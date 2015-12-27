/*
 * Copyright (C) 2015 Pavel Kirienko <pavel.kirienko@gmail.com>
 */

#include <zubax_chibios/os.hpp>
#include <hal.h>
#include <unistd.h>
#include <type_traits>
#include <algorithm>
#include <new>
#include "can_bus.hpp"


namespace can
{
namespace
{

constexpr unsigned IRQPriority = CORTEX_MAX_KERNEL_PRIORITY;
constexpr unsigned NumTxMailboxes = 3;

chibios_rt::Mutex mutex_;


class RxQueue
{
    static constexpr unsigned Capacity = 16;

    RxFrame buf_[Capacity];
    std::uint8_t in_ = 0;
    std::uint8_t out_ = 0;
    std::uint8_t len_ = 0;
    std::uint32_t overflow_cnt_ = 0;

    void registerOverflow()
    {
        if (overflow_cnt_ < 0xFFFFFFFF)
        {
            overflow_cnt_++;
        }
    }

public:
    void push(const RxFrame& frame)
    {
        buf_[in_] = frame;
        in_++;
        if (in_ >= Capacity)
        {
            in_ = 0;
        }
        len_++;
        if (len_ > Capacity)
        {
            len_ = Capacity;
            registerOverflow();
            out_++;
            if (out_ >= Capacity)
            {
                out_ = 0;
            }
        }
    }

    void pop(RxFrame& out_frame)
    {
        if (len_ > 0)
        {
            out_frame = buf_[out_];
            out_++;
            if (out_ >= Capacity)
            {
                out_ = 0;
            }
            len_--;
        }
        else { assert(0); }
    }

    void reset()
    {
        in_ = 0;
        out_ = 0;
        len_ = 0;
        overflow_cnt_ = 0;
    }

    unsigned getLength() const { return len_; }

    std::uint32_t getOverflowCount() const { return overflow_cnt_; }
};


struct Timings
{
    std::uint16_t prescaler = 0;
    std::uint8_t sjw = 0;
    std::uint8_t bs1 = 0;
    std::uint8_t bs2 = 0;
};


struct TxItem
{
    Frame frame;
    bool pending = false;
};

/*
 * Internal functions
 */
int computeTimings(const std::uint32_t target_bitrate, Timings& out_timings)
{
    if (target_bitrate < 1)
    {
        return -ErrInvalidBitRate;
    }

    /*
     * Hardware configuration
     */
    const std::uint32_t pclk = STM32_PCLK1;

    static const int MaxBS1 = 16;
    static const int MaxBS2 = 8;

    /*
     * Ref. "Automatic Baudrate Detection in CANopen Networks", U. Koppe, MicroControl GmbH & Co. KG
     *      CAN in Automation, 2003
     *
     * According to the source, optimal quanta per bit are:
     *   Bitrate        Optimal Maximum
     *   1000 kbps      8       10
     *   500  kbps      16      17
     *   250  kbps      16      17
     *   125  kbps      16      17
     */
    const int max_quanta_per_bit = (target_bitrate >= 1000000) ? 10 : 17;

    assert(max_quanta_per_bit <= (MaxBS1 + MaxBS2));

    static const int MaxSamplePointLocation = 900;

    /*
     * Computing (prescaler * BS):
     *   BITRATE = 1 / (PRESCALER * (1 / PCLK) * (1 + BS1 + BS2))       -- See the Reference Manual
     *   BITRATE = PCLK / (PRESCALER * (1 + BS1 + BS2))                 -- Simplified
     * let:
     *   BS = 1 + BS1 + BS2                                             -- Number of time quanta per bit
     *   PRESCALER_BS = PRESCALER * BS
     * ==>
     *   PRESCALER_BS = PCLK / BITRATE
     */
    const std::uint32_t prescaler_bs = pclk / target_bitrate;

    /*
     * Searching for such prescaler value so that the number of quanta per bit is highest.
     */
    std::uint8_t bs1_bs2_sum = max_quanta_per_bit - 1;

    while ((prescaler_bs % (1 + bs1_bs2_sum)) != 0)
    {
        if (bs1_bs2_sum <= 2)
        {
            return -ErrInvalidBitRate;          // No solution
        }
        bs1_bs2_sum--;
    }

    const std::uint32_t prescaler = prescaler_bs / (1 + bs1_bs2_sum);
    if ((prescaler < 1U) || (prescaler > 1024U))
    {
        return -ErrInvalidBitRate;              // No solution
    }

    /*
     * Now we have a constraint: (BS1 + BS2) == bs1_bs2_sum.
     * We need to find the values so that the sample point is as close as possible to the optimal value.
     *
     *   Solve[(1 + bs1)/(1 + bs1 + bs2) == 7/8, bs2]  (* Where 7/8 is 0.875, the recommended sample point location *)
     *   {{bs2 -> (1 + bs1)/7}}
     *
     * Hence:
     *   bs2 = (1 + bs1) / 7
     *   bs1 = (7 * bs1_bs2_sum - 1) / 8
     *
     * Sample point location can be computed as follows:
     *   Sample point location = (1 + bs1) / (1 + bs1 + bs2)
     *
     * Since the optimal solution is so close to the maximum, we prepare two solutions, and then pick the best one:
     *   - With rounding to nearest
     *   - With rounding to zero
     */
    struct BsPair
    {
        std::uint8_t bs1;
        std::uint8_t bs2;
        std::uint16_t sample_point_permill;

        BsPair() :
            bs1(0),
            bs2(0),
            sample_point_permill(0)
        { }

        BsPair(std::uint8_t bs1_bs2_sum, std::uint8_t arg_bs1) :
            bs1(arg_bs1),
            bs2(bs1_bs2_sum - bs1),
            sample_point_permill(1000 * (1 + bs1) / (1 + bs1 + bs2))
        {
            assert(bs1_bs2_sum > arg_bs1);
        }

        bool isValid() const { return (bs1 >= 1) && (bs1 <= MaxBS1) && (bs2 >= 1) && (bs2 <= MaxBS2); }
    };

    BsPair solution(bs1_bs2_sum, ((7 * bs1_bs2_sum - 1) + 4) / 8);      // First attempt with rounding to nearest

    if (solution.sample_point_permill > MaxSamplePointLocation)
    {
        solution = BsPair(bs1_bs2_sum, (7 * bs1_bs2_sum - 1) / 8);      // Second attempt with rounding to zero
    }

    /*
     * Final validation
     * Helpful Python:
     * def sample_point_from_btr(x):
     *     assert 0b0011110010000000111111000000000 & x == 0
     *     ts2,ts1,brp = (x>>20)&7, (x>>16)&15, x&511
     *     return (1+ts1+1)/(1+ts1+1+ts2+1)
     *
     */
    if ((target_bitrate != (pclk / (prescaler * (1 + solution.bs1 + solution.bs2)))) || !solution.isValid())
    {
        assert(0);
        return -ErrLogic;
    }

//    os::lowsyslog("Timings: quanta/bit: %d, sample point location: %.1f%%\n",
//                  int(1 + solution.bs1 + solution.bs2), float(solution.sample_point_permill) / 10.F);

    out_timings.prescaler = std::uint16_t(prescaler - 1U);
    out_timings.sjw = 0;                                        // Which means one
    out_timings.bs1 = std::uint8_t(solution.bs1 - 1);
    out_timings.bs2 = std::uint8_t(solution.bs2 - 1);
    return 0;
}

bool waitMSRINAKBitStateChange(bool target_state)
{
    constexpr unsigned Timeout = 1000;
    for (unsigned wait_ack = 0; wait_ack < Timeout; wait_ack++)
    {
        const bool state = (CAN->MSR & CAN_MSR_INAK) != 0;
        if (state == target_state)
        {
            return true;
        }
        ::usleep(1000);
    }
    return false;
}


class Event
{
    chibios_rt::CounterSemaphore sem_;

public:
    Event() : sem_(0) { }

    void waitForSysTicks(unsigned systicks) { (void)sem_.wait(systicks); }

    void signalI() { sem_.signalI(); }
};

/*
 * Driver state
 */
struct State
{
    std::uint64_t error_cnt       = 0;
    std::uint64_t rx_overflow_cnt = 0;
    std::uint64_t tx_cnt          = 0;
    std::uint64_t rx_cnt          = 0;

    RxQueue rx_queue;
    Event rx_event;
    Event tx_event;
    TxItem pending_tx[NumTxMailboxes];

    std::uint8_t last_hw_error_code = 0;
    std::uint8_t peak_tx_mailbox_index = 0;
    bool had_activity = false;

    const bool loopback;

    State(bool option_loopback) :
        loopback(option_loopback)
    { }

    void pushRxFromISR(const RxFrame& rxf)
    {
        os::CriticalSectionLocker cs_locker;

        rx_queue.push(rxf);
        rx_event.signalI();

        if (!rxf.loopback && !rxf.failed)
        {
            had_activity = true;
            rx_cnt++;
        }
    }
};

State* state_ = nullptr;

/*
 * Interrupt handlers
 */
void handleTxMailboxInterrupt(const std::uint8_t mailbox_index, const bool txok, const ::systime_t timestamp)
{
    assert(mailbox_index < NumTxMailboxes);

    if (txok)
    {
        state_->had_activity = true;
        state_->tx_cnt++;
    }

    auto& txi = state_->pending_tx[mailbox_index];

    if (state_->loopback && txi.pending)
    {
        RxFrame rxf;
        rxf.frame             = txi.frame;
        rxf.loopback          = true;
        rxf.failed            = !txok;
        rxf.timestamp_systick = timestamp;

        state_->pushRxFromISR(rxf);
    }

    txi.pending = false;

    os::CriticalSectionLocker cs_locker;
    state_->tx_event.signalI();
}

void handleRxInterrupt(const std::uint8_t fifo_index, const ::systime_t timestamp)
{
    static constexpr unsigned CAN_RFR_FMP_MASK = 3;

    assert(fifo_index < 2);

    volatile std::uint32_t& rfr_reg = (fifo_index == 0) ? CAN->RF0R : CAN->RF1R;
    if ((rfr_reg & CAN_RFR_FMP_MASK) == 0)
    {
        assert(0);  // Weird, IRQ is here but no data to read
        return;
    }

    /*
     * Register overflow as a hardware error
     */
    if ((rfr_reg & CAN_RF0R_FOVR0) != 0)
    {
        state_->rx_overflow_cnt++;
    }

    /*
     * Read the frame contents
     */
    RxFrame rxf;
    rxf.timestamp_systick = timestamp;

    const auto& rf = CAN->sFIFOMailBox[fifo_index];

    if ((rf.RIR & CAN_RI0R_IDE) == 0)
    {
        rxf.frame.id = Frame::MaskStdID & (rf.RIR >> 21);
    }
    else
    {
        rxf.frame.id = Frame::MaskExtID & (rf.RIR >> 3);
        rxf.frame.id |= Frame::FlagEFF;
    }

    if ((rf.RIR & CAN_RI0R_RTR) != 0)
    {
        rxf.frame.id |= Frame::FlagRTR;
    }

    rxf.frame.dlc = rf.RDTR & 15;

    {
        const std::uint32_t r = rf.RDLR;
        rxf.frame.data[0] = std::uint8_t(0xFF & (r >> 0));
        rxf.frame.data[1] = std::uint8_t(0xFF & (r >> 8));
        rxf.frame.data[2] = std::uint8_t(0xFF & (r >> 16));
        rxf.frame.data[3] = std::uint8_t(0xFF & (r >> 24));
    }
    {
        const std::uint32_t r = rf.RDHR;
        rxf.frame.data[4] = std::uint8_t(0xFF & (r >> 0));
        rxf.frame.data[5] = std::uint8_t(0xFF & (r >> 8));
        rxf.frame.data[6] = std::uint8_t(0xFF & (r >> 16));
        rxf.frame.data[7] = std::uint8_t(0xFF & (r >> 24));
    }

    rfr_reg = CAN_RF0R_RFOM0 | CAN_RF0R_FOVR0 | CAN_RF0R_FULL0;  // Release FIFO entry we just read

    /*
     * Store with timeout into the FIFO buffer and signal update event
     */
    state_->pushRxFromISR(rxf);
}

void handleStatusChangeInterrupt(const ::systime_t timestamp)
{
    CAN->MSR = CAN_MSR_ERRI;        // Clear error

    /*
     * Cancel all transmissions when we reach bus-off state
     */
    if (CAN->ESR & CAN_ESR_BOFF)
    {
        CAN->TSR = CAN_TSR_ABRQ0 | CAN_TSR_ABRQ1 | CAN_TSR_ABRQ2;

        {
            os::CriticalSectionLocker cs_locker;
            state_->tx_event.signalI();
        }

        // Marking TX mailboxes empty
        for (unsigned i = 0; i < NumTxMailboxes; i++)
        {
            auto& tx = state_->pending_tx[i];
            if (tx.pending)
            {
                tx.pending = false;

                // If loopback is enabled, reporting that the transmission has failed.
                if (state_->loopback)
                {
                    RxFrame rxf;
                    rxf.frame = tx.frame;
                    rxf.failed = true;
                    rxf.loopback = true;
                    rxf.timestamp_systick = timestamp;

                    state_->pushRxFromISR(rxf);
                }
            }
        }
    }

    const std::uint8_t lec = std::uint8_t((CAN->ESR & CAN_ESR_LEC) >> 4);
    if (lec != 0)
    {
        state_->last_hw_error_code = lec;
        state_->error_cnt++;
    }

    CAN->ESR = 0;
}

bool canAcceptNewTxFrame(const Frame& frame)
{
    /*
     * We can accept more frames only if the following conditions are satisfied:
     *  - There is at least one TX mailbox free (obvious enough);
     *  - The priority of the new frame is higher than priority of all TX mailboxes.
     */
    {
        static constexpr std::uint32_t TME = CAN_TSR_TME0 | CAN_TSR_TME1 | CAN_TSR_TME2;
        const std::uint32_t tme = CAN->TSR & TME;

        if (tme == TME)     // All TX mailboxes are free (as in freedom).
        {
            return true;
        }

        if (tme == 0)       // All TX mailboxes are busy transmitting.
        {
            return false;
        }
    }

    /*
     * The second condition requires a critical section.
     */
    os::CriticalSectionLocker lock;

    for (unsigned mbx = 0; mbx < NumTxMailboxes; mbx++)
    {
        if (state_->pending_tx[mbx].pending && !frame.priorityHigherThan(state_->pending_tx[mbx].frame))
        {
            return false;       // There's a mailbox whose priority is higher or equal the priority of the new frame.
        }
    }

    return true;                // This new frame will be added to a free TX mailbox in the next @ref send().
}

int loadTxMailbox(const Frame& frame)
{
    if (frame.isErrorFrame() || frame.dlc > 8)
    {
        return -ErrUnsupportedFrame;
    }

    /*
     * Normally we should perform the same check as in @ref canAcceptNewTxFrame(), because
     * it is possible that the highest-priority frame between select() and send() could have been
     * replaced with a lower priority one due to TX timeout. But we don't do this check because:
     *
     *  - It is a highly unlikely scenario.
     *
     *  - Frames do not timeout on a properly functioning bus. Since frames do not timeout, the new
     *    frame can only have higher priority, which doesn't break the logic.
     *
     *  - If high-priority frames are timing out in the TX queue, there's probably a lot of other
     *    issues to take care of before this one becomes relevant.
     *
     *  - It takes CPU time. Not just CPU time, but critical section time, which is expensive.
     */
    os::CriticalSectionLocker cs_lock;

    /*
     * Seeking for an empty slot
     */
    std::uint8_t txmailbox = 0xFF;
    if ((CAN->TSR & CAN_TSR_TME0) == CAN_TSR_TME0)
    {
        txmailbox = 0;
    }
    else if ((CAN->TSR & CAN_TSR_TME1) == CAN_TSR_TME1)
    {
        txmailbox = 1;
    }
    else if ((CAN->TSR & CAN_TSR_TME2) == CAN_TSR_TME2)
    {
        txmailbox = 2;
    }
    else
    {
        return 0;       // No transmission for you.
    }

    state_->peak_tx_mailbox_index = std::max(state_->peak_tx_mailbox_index, txmailbox);    // Statistics

    /*
     * Setting up the mailbox
     */
    auto& mb = CAN->sTxMailBox[txmailbox];
    if (frame.isExtended())
    {
        mb.TIR = ((frame.id & Frame::MaskExtID) << 3) | CAN_TI0R_IDE;
    }
    else
    {
        mb.TIR = ((frame.id & Frame::MaskStdID) << 21);
    }

    if (frame.isRemoteTransmissionRequest())
    {
        mb.TIR |= CAN_TI0R_RTR;
    }

    mb.TDTR = frame.dlc;

    mb.TDHR = (std::uint32_t(frame.data[7]) << 24) |
              (std::uint32_t(frame.data[6]) << 16) |
              (std::uint32_t(frame.data[5]) << 8)  |
              (std::uint32_t(frame.data[4]) << 0);
    mb.TDLR = (std::uint32_t(frame.data[3]) << 24) |
              (std::uint32_t(frame.data[2]) << 16) |
              (std::uint32_t(frame.data[1]) << 8)  |
              (std::uint32_t(frame.data[0]) << 0);

    mb.TIR |= CAN_TI0R_TXRQ;  // Go. Transmission starts here.

    /*
     * Registering the pending transmission
     */
    auto& txi = state_->pending_tx[txmailbox];
    txi.frame   = frame;
    txi.pending = true;
    return 1;
}

} // namespace

bool Frame::priorityHigherThan(const Frame& rhs) const
{
    const uint32_t clean_id     = id     & MaskExtID;
    const uint32_t rhs_clean_id = rhs.id & MaskExtID;

    /*
     * STD vs EXT - if 11 most significant bits are the same, EXT loses.
     */
    const bool ext     = id     & FlagEFF;
    const bool rhs_ext = rhs.id & FlagEFF;
    if (ext != rhs_ext)
    {
        const uint32_t arb11     = ext     ? (clean_id >> 18)     : clean_id;
        const uint32_t rhs_arb11 = rhs_ext ? (rhs_clean_id >> 18) : rhs_clean_id;
        if (arb11 != rhs_arb11)
        {
            return arb11 < rhs_arb11;
        }
        else
        {
            return rhs_ext;
        }
    }

    /*
     * RTR vs Data frame - if frame identifiers and frame types are the same, RTR loses.
     */
    const bool rtr     = id     & FlagRTR;
    const bool rhs_rtr = rhs.id & FlagRTR;
    if (clean_id == rhs_clean_id && rtr != rhs_rtr)
    {
        return rhs_rtr;
    }

    /*
     * Plain ID arbitration - greater value loses.
     */
    return clean_id < rhs_clean_id;
}

int start(std::uint32_t bitrate, unsigned options)
{
    os::MutexLocker mutex_locker(mutex_);

    EXECUTE_ONCE_NON_THREAD_SAFE
    {
        os::CriticalSectionLocker cs_lock;

        RCC->APB1ENR  |=  RCC_APB1ENR_CANEN;
        RCC->APB1RSTR |=  RCC_APB1RSTR_CANRST;
        RCC->APB1RSTR &= ~RCC_APB1RSTR_CANRST;
        nvicEnableVector(CEC_CAN_IRQn, IRQPriority);
    }

    /*
     * We need to silence the controller in the first order, otherwise it may interfere with the following operations.
     */
    {
        os::CriticalSectionLocker cs_lock;

        CAN->MCR &= ~CAN_MCR_SLEEP;     // Exit sleep mode
        CAN->MCR |= CAN_MCR_INRQ;       // Request init
        CAN->IER = 0;                   // Disable interrupts while initialization is in progress
    }

    if (!waitMSRINAKBitStateChange(true))
    {
        return -ErrMsrInakNotSet;
    }

    /*
     * Resetting driver state - CAN interrupts are disabled, so it's safe to modify it now
     */
    static std::aligned_storage_t<sizeof(State), alignof(State)> _state_storage;
    state_ = new (&_state_storage) State((options & OptionLoopback) != 0);

    /*
     * CAN timings for this bitrate
     */
    Timings timings;
    const int timings_res = computeTimings(bitrate, timings);
    if (timings_res < 0)
    {
        return timings_res;
    }
//    os::lowsyslog("Timings: presc=%u sjw=%u bs1=%u bs2=%u\n",
//                  unsigned(timings.prescaler), unsigned(timings.sjw), unsigned(timings.bs1), unsigned(timings.bs2));

    /*
     * Hardware initialization (the hardware has already confirmed initialization mode, see above)
     */
    CAN->MCR = CAN_MCR_ABOM | CAN_MCR_AWUM | CAN_MCR_INRQ;  // RM page 648

    CAN->BTR = ((timings.sjw & 3U)  << 24) |
               ((timings.bs1 & 15U) << 16) |
               ((timings.bs2 & 7U)  << 20) |
               (timings.prescaler & 1023U) |
               (((options & OptionSilentMode) != 0) ? CAN_BTR_SILM : 0);

    // From now on the interrupts will be re-enabled
    CAN->IER = CAN_IER_TMEIE |          // TX mailbox empty
               CAN_IER_FMPIE0 |         // RX FIFO 0 is not empty
               CAN_IER_FMPIE1 |         // RX FIFO 1 is not empty
               CAN_IER_ERRIE |          // General error IRQ
               CAN_IER_LECIE |          // Last error code change
               CAN_IER_BOFIE;           // Bus-off reached

    CAN->MCR &= ~CAN_MCR_INRQ;          // Leave init mode

    if (!waitMSRINAKBitStateChange(false))
    {
        return -ErrMsrInakNotCleared;
    }

    os::CriticalSectionLocker cs_lock;  // Entering a critical section for the rest of initialization

    /*
     * Default filter configuration
     */
    CAN->FMR |= CAN_FMR_FINIT;

    CAN->FMR &= 0xFFFFC0F1;
    CAN->FMR |= static_cast<std::uint32_t>(27) << 8;    // Refer to the bxCAN macrocell documentation for explanation

    CAN->FFA1R = 0;                     // All assigned to FIFO0 by default
    CAN->FM1R = 0;                      // Indentifier Mask mode

    CAN->FS1R = 0x1fff;
    CAN->sFilterRegister[0].FR1 = 0;
    CAN->sFilterRegister[0].FR2 = 0;
    CAN->FA1R = 1;

    CAN->FMR &= ~CAN_FMR_FINIT;

    return 0;
}

void stop()
{
    os::MutexLocker mutex_locker(mutex_);
    os::CriticalSectionLocker cs_lock;

    CAN->IER = 0;                                           // Disable interrupts
    CAN->MCR = CAN_MCR_SLEEP | CAN_MCR_RESET;               // Force software reset of the macrocell

    NVIC_ClearPendingIRQ(static_cast<IRQn_Type>(CEC_CAN_IRQn));
}

int send(const Frame& frame, std::uint16_t timeout_ms)
{
    os::MutexLocker mutex_locker(mutex_);

    const auto started_at = chVTGetSystemTimeX();

    while (true)
    {
        if (canAcceptNewTxFrame(frame))
        {
            return loadTxMailbox(frame);
        }

        // Blocking until next event or timeout
        const auto elapsed = chVTTimeElapsedSinceX(started_at);
        if (elapsed >= MS2ST(timeout_ms))
        {
            return 0;
        }
        state_->tx_event.waitForSysTicks(MS2ST(timeout_ms) - elapsed);
    }

    return -1;
}

int receive(RxFrame& out_frame, std::uint16_t timeout_ms)
{
    os::MutexLocker mutex_locker(mutex_);

    const auto started_at = chVTGetSystemTimeX();

    while (true)
    {
        {
            os::CriticalSectionLocker cs_locker;
            if (state_->rx_queue.getLength() > 0)
            {
                state_->rx_queue.pop(out_frame);
                return 1;
            }
        }

        // Blocking until next event or timeout
        const auto elapsed = chVTTimeElapsedSinceX(started_at);
        if (elapsed >= MS2ST(timeout_ms))
        {
            return 0;
        }
        state_->rx_event.waitForSysTicks(MS2ST(timeout_ms) - elapsed);
    }

    return -1;
}

}

extern "C"
{

using namespace can;

CH_IRQ_HANDLER(STM32_CAN1_UNIFIED_HANDLER)
{
    CH_IRQ_PROLOGUE();

    const auto timestamp = chVTGetSystemTimeX();
    assert(state_ != nullptr);

    /*
     * TX interrupt handling
     * TXOK == false means that there was a hardware failure
     */
    const auto tsr = CAN->TSR;
    if (tsr & (CAN_TSR_RQCP0 | CAN_TSR_RQCP1 | CAN_TSR_RQCP2))
    {
        if (tsr & CAN_TSR_RQCP0)
        {
            const bool txok = (tsr & CAN_TSR_TXOK0) != 0;
            CAN->TSR = CAN_TSR_RQCP0;
            handleTxMailboxInterrupt(0, txok, timestamp);
        }
        if (tsr & CAN_TSR_RQCP1)
        {
            const bool txok = (tsr & CAN_TSR_TXOK1) != 0;
            CAN->TSR = CAN_TSR_RQCP1;
            handleTxMailboxInterrupt(1, txok, timestamp);
        }
        if (tsr & CAN_TSR_RQCP2)
        {
            const bool txok = (tsr & CAN_TSR_TXOK2) != 0;
            CAN->TSR = CAN_TSR_RQCP2;
            handleTxMailboxInterrupt(2, txok, timestamp);
        }
    }

    /*
     * RX interrupt handling
     */
    while ((CAN->RF0R & CAN_RF0R_FMP0) != 0)
    {
        handleRxInterrupt(0, timestamp);
    }
    while ((CAN->RF1R & CAN_RF1R_FMP1) != 0)
    {
        handleRxInterrupt(1, timestamp);
    }

    /*
     * Status change interrupt handling
     */
    if (CAN->MSR & CAN_MSR_ERRI)
    {
        handleStatusChangeInterrupt(timestamp);
    }

    CH_IRQ_EPILOGUE();
}

}