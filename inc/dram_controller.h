#ifndef DRAM_H
#define DRAM_H

#include "memory_class.h"

// DRAM configuration
#define DRAM_CHANNEL_WIDTH 8 // 8B
#define DRAM_WQ_SIZE 64
#define DRAM_RQ_SIZE 64

#define tRP_DRAM_NANOSECONDS  12.5
#define tRCD_DRAM_NANOSECONDS 12.5
#define tCAS_DRAM_NANOSECONDS 12.5

// the data bus must wait this amount of time when switching between reads and writes, and vice versa
#define DRAM_DBUS_TURN_AROUND_TIME ((15*CPU_FREQ)/2000) // 7.5 ns
// NOTE: DRAM_MTPS/DRAM_DBUS_RETURN_TIME/tRP/tRCD/tCAS used to be plain
// globals here (and in memory_class.h), shared by every MEMORY_CONTROLLER
// instance. That made it impossible for two controllers (e.g. a DRAM-cache
// "near" device and a "far" backing store) to run different timing
// profiles at the same time -- see docs/gem5_to_champsim_mapping.md. They
// are now per-instance members below, set via set_timing(). This is the
// only behavioral change: main.cc now calls set_timing() explicitly on
// each MEMORY_CONTROLLER instance with the same values it used to assign
// to the old globals, so existing single-DRAM behavior is unchanged.

// these values control when to send out a burst of writes
#define DRAM_WRITE_HIGH_WM    ((DRAM_WQ_SIZE*7)>>3) // 7/8th
#define DRAM_WRITE_LOW_WM     ((DRAM_WQ_SIZE*3)>>2) // 6/8th
#define MIN_DRAM_WRITES_PER_SWITCH (DRAM_WQ_SIZE*1/4)

// DRAM
class MEMORY_CONTROLLER : public MEMORY {
  public:
    const string NAME;

    DRAM_ARRAY dram_array[DRAM_CHANNELS][DRAM_RANKS][DRAM_BANKS];
    uint64_t dbus_cycle_available[DRAM_CHANNELS], dbus_cycle_congested[DRAM_CHANNELS], dbus_congested[NUM_TYPES+1][NUM_TYPES+1];
    uint64_t bank_cycle_available[DRAM_CHANNELS][DRAM_RANKS][DRAM_BANKS];
    uint8_t  do_write, write_mode[DRAM_CHANNELS];
    uint32_t processed_writes, scheduled_reads[DRAM_CHANNELS], scheduled_writes[DRAM_CHANNELS];
    int fill_level;

    // Per-instance DRAM timing (see the NOTE above DRAM_DBUS_TURN_AROUND_TIME).
    // Zero-initialized by the constructor; callers must call set_timing()
    // before relying on real latency behavior (main.cc does this for
    // uncore's controllers; tests must do it themselves).
    uint32_t tRP, tRCD, tCAS, DRAM_MTPS, DRAM_DBUS_RETURN_TIME;

    BANK_REQUEST bank_request[DRAM_CHANNELS][DRAM_RANKS][DRAM_BANKS];

    // queues
    PACKET_QUEUE WQ[DRAM_CHANNELS], RQ[DRAM_CHANNELS];

    // constructor
    MEMORY_CONTROLLER(string v1) : NAME (v1) {
        tRP = 0;
        tRCD = 0;
        tCAS = 0;
        DRAM_MTPS = 0;
        DRAM_DBUS_RETURN_TIME = 0;

        for (uint32_t i=0; i<NUM_TYPES+1; i++) {
            for (uint32_t j=0; j<NUM_TYPES+1; j++) {
                dbus_congested[i][j] = 0;
            }
        }
        do_write = 0;
        processed_writes = 0;
        for (uint32_t i=0; i<DRAM_CHANNELS; i++) {
            dbus_cycle_available[i] = 0;
            dbus_cycle_congested[i] = 0;
            write_mode[i] = 0;
            scheduled_reads[i] = 0;
            scheduled_writes[i] = 0;

            for (uint32_t j=0; j<DRAM_RANKS; j++) {
                for (uint32_t k=0; k<DRAM_BANKS; k++)
                    bank_cycle_available[i][j][k] = 0;
            }

            WQ[i].NAME = "DRAM_WQ" + to_string(i);
            WQ[i].SIZE = DRAM_WQ_SIZE;
            WQ[i].entry = new PACKET [DRAM_WQ_SIZE];

            RQ[i].NAME = "DRAM_RQ" + to_string(i);
            RQ[i].SIZE = DRAM_RQ_SIZE;
            RQ[i].entry = new PACKET [DRAM_RQ_SIZE];
        }

        fill_level = FILL_DRAM;
    };

    // destructor
    ~MEMORY_CONTROLLER() {

    };

    // Sets this instance's DRAM timing independently of any other
    // MEMORY_CONTROLLER instance. See the per-instance-timing NOTE above.
    void set_timing(uint32_t v_tRP, uint32_t v_tRCD, uint32_t v_tCAS,
                     uint32_t v_DRAM_MTPS, uint32_t v_DRAM_DBUS_RETURN_TIME) {
        tRP = v_tRP;
        tRCD = v_tRCD;
        tCAS = v_tCAS;
        DRAM_MTPS = v_DRAM_MTPS;
        DRAM_DBUS_RETURN_TIME = v_DRAM_DBUS_RETURN_TIME;
    }

    // functions
    int  add_rq(PACKET *packet),
         add_wq(PACKET *packet),
         add_pq(PACKET *packet);

    void return_data(PACKET *packet),
         operate(),
         increment_WQ_FULL(uint64_t address);

    uint32_t get_occupancy(uint8_t queue_type, uint64_t address),
             get_size(uint8_t queue_type, uint64_t address);

    void schedule(PACKET_QUEUE *queue), process(PACKET_QUEUE *queue),
         update_schedule_cycle(PACKET_QUEUE *queue),
         update_process_cycle(PACKET_QUEUE *queue),
         reset_remain_requests(PACKET_QUEUE *queue, uint32_t channel);

    uint32_t dram_get_channel(uint64_t address),
             dram_get_rank   (uint64_t address),
             dram_get_bank   (uint64_t address),
             dram_get_row    (uint64_t address),
             dram_get_column (uint64_t address),
             drc_check_hit (uint64_t address, uint32_t cpu, uint32_t channel, uint32_t rank, uint32_t bank, uint32_t row);

    uint64_t get_bank_earliest_cycle();

    int check_dram_queue(PACKET_QUEUE *queue, PACKET *packet);
};

#endif
