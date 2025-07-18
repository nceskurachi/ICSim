#ifndef ICSIM_H
#define ICSIM_H

#include <stdint.h>
#include <SDL2/SDL.h>
#include <linux/can.h>


/* === Constants === */

// Display dimensions
#define SCREEN_WIDTH 692
#define SCREEN_HEIGHT 329

// Door status
#define DOOR_LOCKED 0
#define DOOR_UNLOCKED 1

// ON/OFF definitions
#define OFF 0
#define ON 1

// CAN ID and byte position (default)
#define DEFAULT_DOOR_ID 411        // 0x19B
#define DEFAULT_DOOR_BYTE 2
#define DEFAULT_SIGNAL_ID 392      // 0x188
#define DEFAULT_SIGNAL_BYTE 0
#define DEFAULT_SPEED_ID 580       // 0x244
#define DEFAULT_SPEED_BYTE 3       // bytes 3,4

#define CAN_DOOR1_LOCK 1
#define CAN_DOOR2_LOCK 2
#define CAN_DOOR3_LOCK 4
#define CAN_DOOR4_LOCK 8

#define CAN_LEFT_SIGNAL 1
#define CAN_RIGHT_SIGNAL 2

// Model（BMW X1）
#define MODEL_BMW_X1_SPEED_ID 0x1B4
#define MODEL_BMW_X1_SPEED_BYTE 0
#define MODEL_BMW_X1_RPM_ID 0x0AA
#define MODEL_BMW_X1_RPM_BYTE 4

// Display refresh rate
#define TARGET_FPS 60
#define FRAME_DELAY_MS (1000 / TARGET_FPS)

// UDS (Unified Diagnostic Services)
#define UDS_SECURITY_REQ       0x27
#define UDS_SECURITY_REQ_SEED  0x01
#define UDS_SECURITY_REQ_KEY   0x02
#define UDS_DIAG_ID            0x7DF
#define EXPECTED_KEY           0x5A


/* === Structures === */

// Define the car state structure
typedef struct {
  long speed;
  int door_status[4];
  int turn_status[2];
  int lock_status; // ON / OFF
  Uint32 unlock_time; 
} CarState;

// Define the redraw flags structure
typedef struct {
  int speed_redraw;
  int doors_redraw;
  int turn_redraw;
  int lock_redraw;
} RedrawFlags;

// Security context (UDS SecurityAccess)
typedef struct {
  enum { SEC_STATE_IDLE, SEC_STATE_WAIT_KEY } state;
  Uint8 seed;
  Uint32 seed_sent_time;
  Uint32 timeout_ms;
} SecurityContext;

/* === Global Variables（See icsim.c）=== */

extern CarState car_state;
extern SDL_Renderer *renderer;

/* === Prototypes === */

//  Initialization
void init_car_state(void);

// Update functions
void update_speed_status(struct canfd_frame *cf, int maxdlen);
void update_door_status(struct canfd_frame *cf, int maxdlen);
void update_signal_status(struct canfd_frame *cf, int maxdlen);
void update_security_status(struct canfd_frame *cf, int maxdlen, int can_fd, SecurityContext* ctx);

// Rendering functions
void blank_ic(void);
void update_speed(CarState* state);
void update_doors(CarState* state);
void update_turn_signals(CarState* state);
void update_lock_icon(CarState* state);
void redraw_ic(CarState* snapshot, RedrawFlags* flags);

//  Redraw flags
void update_redraw_flags(CarState* prev, CarState* curr, RedrawFlags* flags);

// UDS (Unified Diagnostic Services)
int send_can_response(uint32_t can_id, uint8_t* data, uint8_t len, int can_fd);
int send_canfd_response(uint32_t can_id, uint8_t* data, uint8_t len, int can_fd);
Uint8 generate_seed(void);
Uint8 calculate_key(Uint8 seed);

// Utility functions
char* get_data(char *fname);
void Usage(char *msg);

#endif // ICSIM_H
