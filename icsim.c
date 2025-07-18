/*
 * Instrument cluster simulator
 *
 * (c) 2014 Open Garages - Craig Smith <craig@theialabs.com>
 * (c) 2025 NCES - Ryo Kurachi <kurachi@nces.i.nagoya-u.ac.jp>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <locale.h>
#include <errno.h>

#include "lib.h"
#include "icsim.h"

#ifndef DATA_DIR
#define DATA_DIR "./data/"  // Needs trailing slash
#endif


const int canfd_on = 1;
int debug = 0;
int randomize = 0;
int seed = 0;
int door_pos = DEFAULT_DOOR_BYTE;
int signal_pos = DEFAULT_SIGNAL_BYTE;
int speed_pos = DEFAULT_SPEED_BYTE;
char *model = NULL;
char data_file[256];
int running = 1;
canid_t door_id = DEFAULT_DOOR_ID;
canid_t signal_id = DEFAULT_SIGNAL_ID;
canid_t speed_id = DEFAULT_SPEED_ID;

SDL_Renderer *renderer = NULL;
SDL_Texture *base_texture = NULL;
SDL_Texture *needle_tex = NULL;
SDL_Texture *sprite_tex = NULL;
SDL_Texture *lock_tex = NULL;
SDL_Texture *unlock_tex = NULL;

SDL_Rect speed_rect;
SDL_Thread* can_thread = NULL;
SDL_mutex* state_mutex;

// Global car state
CarState car_state;
// Redraw flags
RedrawFlags redraw_flags = {1, 1, 1, 1};

// Security context for UDS Security Access
SecurityContext sec_ctx = {
  .state = SEC_STATE_IDLE,
  .seed = 0,
  .seed_sent_time = 0,
  .timeout_ms = 10000  // 10 seconds
};

// Simple map function
long map(long x, long in_min, long in_max, long out_min, long out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// Adds data dir to file name
// Uses a single pointer so not to have a memory leak
// returns point to data_files or NULL if append is too large
char *get_data(char *fname) {
  if(strlen(DATA_DIR) + strlen(fname) > 255) return NULL;
  strncpy(data_file, DATA_DIR, 255);
  strncat(data_file, fname, 255-strlen(data_file));
  return data_file;
}

/* Default vehicle state */
void init_car_state() {
  car_state.speed = 0;
  car_state.door_status[0] = DOOR_LOCKED;
  car_state.door_status[1] = DOOR_LOCKED;
  car_state.door_status[2] = DOOR_LOCKED;
  car_state.door_status[3] = DOOR_LOCKED;
  car_state.turn_status[0] = OFF;
  car_state.turn_status[1] = OFF;
  car_state.lock_status = ON;
}

/* Empty IC */
void blank_ic() {
  SDL_RenderCopy(renderer, base_texture, NULL, NULL);
}

/* Updates speedo */
void update_speed(CarState* state) {
  SDL_Point center;
  double angle = 0;
  center.x = 135;
  center.y = 20;
  angle = map(state->speed, 0, 280, 0, 180);
  if(angle < 0) angle = 0;
  if(angle > 180) angle = 180;
  SDL_RenderCopyEx(renderer, needle_tex, NULL, &speed_rect, angle, &center, SDL_FLIP_NONE);
}

/* Updates door unlocks simulated by door open icons */
void update_doors(CarState* state) {
  SDL_Rect update, pos;
  // If any door is unlocked, draw the red body and the specific open doors
  const SDL_Point sprite_coords[4] = {
    {420, 263}, // Front Left
    {484, 261}, // Front Right
    {420, 284}, // Rear Left
    {484, 287}  // Rear Right
  };
  const SDL_Point offset = {22, 22};
  const SDL_Point body_sprite = {440, 239};
  const SDL_Point body_size   = {45, 83};
  const SDL_Point door_size   = {21, 22};

  // Draw the white body first
  SDL_Rect src_white = {body_sprite.x, body_sprite.y, body_size.x, body_size.y};
  SDL_Rect dst_white = {src_white.x - offset.x, src_white.y - offset.y, src_white.w, src_white.h};
  SDL_RenderCopy(renderer, sprite_tex, &src_white, &dst_white);

  // Red body is drawn if any door is unlocked
  for (int i = 0; i < 4; ++i) {
    if (state->door_status[i] == DOOR_UNLOCKED) {
      SDL_Rect src = {body_sprite.x, body_sprite.y, body_size.x, body_size.y};
      SDL_Rect dst = {src.x - offset.x, src.y - offset.y, src.w, src.h};
      SDL_RenderCopy(renderer, sprite_tex, &src, &dst);
      break;
    }
  }

  // Draw each door that is unlocked
  for (int i = 0; i < 4; ++i) {
    if (state->door_status[i] == DOOR_UNLOCKED) {
      SDL_Rect src = {sprite_coords[i].x, sprite_coords[i].y, door_size.x, door_size.y};
      SDL_Rect dst = {src.x - offset.x, src.y - offset.y, src.w, src.h};
      SDL_RenderCopy(renderer, sprite_tex, &src, &dst);
    }
  }
}

/* Updates turn signals */
void update_turn_signals(CarState* state) {
  SDL_Rect left, right, lpos, rpos;
  left.x = 213;
  left.y = 51;
  left.w = 45;
  left.h = 45;
  memcpy(&right, &left, sizeof(SDL_Rect));
  right.x = 482;
  memcpy(&lpos, &left, sizeof(SDL_Rect));
  memcpy(&rpos, &right, sizeof(SDL_Rect));
  lpos.x -= 22;
  lpos.y -= 22;
  rpos.x -= 22;
  rpos.y -= 22;

  if(state->turn_status[0] == ON) {
	SDL_RenderCopy(renderer, sprite_tex, &left, &lpos);
  }
  if(state->turn_status[1] == ON) {
	SDL_RenderCopy(renderer, sprite_tex, &right, &rpos);
  }
}

void update_lock_icon(CarState* state) {
  SDL_Texture* tex = (state->lock_status == OFF) ? unlock_tex : lock_tex;
  int tex_w = 0, tex_h = 0;
  int win_w = 0, win_h = 0;

  // Get the texture size
  if (SDL_QueryTexture(tex, NULL, NULL, &tex_w, &tex_h) != 0) {
    fprintf(stderr, "SDL_QueryTexture failed: %s\n", SDL_GetError());
    return;
  }

  // Get the window size (output size of the render target)
  if (SDL_GetRendererOutputSize(renderer, &win_w, &win_h) != 0) {
    fprintf(stderr, "SDL_GetRendererOutputSize failed: %s\n", SDL_GetError());
    return;
  }

  // Padding (displayed slightly inward from the bottom right)
  const int padding = 10;

  // Calculate the drawing position (bottom right)
  SDL_Rect icon_rect = {
    .x = win_w - tex_w - padding,
    .y = win_h - tex_h - padding,
    .w = tex_w,
    .h = tex_h
  };

  SDL_RenderCopy(renderer, tex, NULL, &icon_rect);
}


/* Redraws the IC updating everything */
void redraw_ic(CarState* snapshot, RedrawFlags* flags) {
  SDL_Rect lock_icon_rect = { 600, 20, 32, 32 };

  // 1. Clear the screen with the base background texture
  blank_ic();
  
  // 2. Draw all dynamic components on top based on their current state
  update_speed(snapshot);
  update_doors(snapshot);
  update_turn_signals(snapshot);
  update_lock_icon(snapshot);
}


/* Parses CAN fram and updates current_speed */
void update_speed_status(struct canfd_frame *cf, int maxdlen) {
  int len = (cf->len > maxdlen) ? maxdlen : cf->len;
  if(len < speed_pos + 1) return;
  if (model) {
	  if (!strncmp(model, "bmw", 3)) {
		  car_state.speed = (((cf->data[speed_pos + 1] - 208) * 256) + cf->data[speed_pos]) / 16;
	  }
  } else {
	  int speed = cf->data[speed_pos] << 8;
	  speed += cf->data[speed_pos + 1];
	  speed = speed / 100; // speed in kilometers
	  car_state.speed = speed * 0.6213751; // mph
  }
}

/* Parses CAN frame and updates turn signal status */
void update_signal_status(struct canfd_frame *cf, int maxdlen) {
  int len = (cf->len > maxdlen) ? maxdlen : cf->len;
  if(len < signal_pos) return;
  if(cf->data[signal_pos] & CAN_LEFT_SIGNAL) {
    car_state.turn_status[0] = ON;
  } else {
    car_state.turn_status[0] = OFF;
  }
  if(cf->data[signal_pos] & CAN_RIGHT_SIGNAL) {
    car_state.turn_status[1] = ON;
  } else {
    car_state.turn_status[1] = OFF;
  }
}

/* Parses CAN frame and updates door status */
void update_door_status(struct canfd_frame *cf, int maxdlen) {
  int len = (cf->len > maxdlen) ? maxdlen : cf->len;
  if(len < door_pos) return;
  if(cf->data[door_pos] & CAN_DOOR1_LOCK) {
	car_state.door_status[0] = DOOR_LOCKED;
  } else {
	car_state.door_status[0] = DOOR_UNLOCKED;
  }
  if(cf->data[door_pos] & CAN_DOOR2_LOCK) {
	car_state.door_status[1] = DOOR_LOCKED;
  } else {
	car_state.door_status[1] = DOOR_UNLOCKED;
  }
  if(cf->data[door_pos] & CAN_DOOR3_LOCK) {
	car_state.door_status[2] = DOOR_LOCKED;
  } else {
	car_state.door_status[2] = DOOR_UNLOCKED;
  }
  if(cf->data[door_pos] & CAN_DOOR4_LOCK) {
	car_state.door_status[3] = DOOR_LOCKED;
  } else {
	car_state.door_status[3] = DOOR_UNLOCKED;
  }
}

int can_receive_thread(void* arg) {
  int can_fd = *(int*)arg;
  struct canfd_frame frame;
  struct sockaddr_can addr;
  struct msghdr msg;
  struct iovec iov = {.iov_base = &frame, .iov_len = sizeof(frame)};
  char ctrlmsg[CMSG_SPACE(sizeof(struct timeval)) + CMSG_SPACE(sizeof(__u32))];
  extern SecurityContext sec_ctx;

  msg.msg_name = &addr;
  msg.msg_namelen = sizeof(addr);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ctrlmsg;
  msg.msg_controllen = sizeof(ctrlmsg);
  msg.msg_flags = 0;

  while (running) {
    int nbytes = recvmsg(can_fd, &msg, 0);
    if (nbytes <= 0) continue;

    SDL_LockMutex(state_mutex);
    if (frame.can_id == DEFAULT_DOOR_ID) update_door_status(&frame, CAN_MTU);
    if (frame.can_id == DEFAULT_SIGNAL_ID) update_signal_status(&frame, CAN_MTU);
    if (frame.can_id == DEFAULT_SPEED_ID) update_speed_status(&frame, CAN_MTU);
    if (frame.can_id == UDS_DIAG_ID) update_security_status(&frame, CAN_MTU, can_fd, &sec_ctx);
    SDL_UnlockMutex(state_mutex);
    }
  return 0;
}

void Usage(char *msg) {
  if(msg) printf("%s\n", msg);
  printf("Usage: icsim [options] <can>\n");
  printf("\t-r\trandomize IDs\n");
  printf("\t-s\tseed value\n");
  printf("\t-d\tdebug mode\n");
  printf("\t-m\tmodel NAME  (Ex: -m bmw)\n");
  exit(1);
}


void update_redraw_flags(CarState* prev, CarState* curr, RedrawFlags* flags) {
  flags->speed_redraw = (prev->speed != curr->speed);

  flags->doors_redraw = 0;
  for (int i = 0; i < 4; ++i)
    if (prev->door_status[i] != curr->door_status[i])
      flags->doors_redraw = 1;

  flags->turn_redraw = 0;
  for (int i = 0; i < 2; ++i)
    if (prev->turn_status[i] != curr->turn_status[i])
      flags->turn_redraw = 1;

  flags->lock_redraw = (prev->lock_status != curr->lock_status);
}

int send_can_response(uint32_t can_id, uint8_t* data, uint8_t len, int can_fd) {
    struct can_frame resp;
    memset(&resp, 0, sizeof(resp));
    resp.can_id = can_id;
    resp.can_dlc = len;
    memcpy(resp.data, data, len);

    ssize_t n = write(can_fd, &resp, sizeof(resp));
    if (n < 0) {
        perror("[ERROR] write failed");
    }
    return n;
}

int send_canfd_response(uint32_t can_id, uint8_t* data, uint8_t len, int can_fd) {
    if (len > CANFD_MAX_DLEN) {
        fprintf(stderr, "[ERROR] CAN FD payload too large (%d bytes)\n", len);
        return;
    }

    struct canfd_frame frame;
    memset(&frame, 0, sizeof(frame));

    frame.can_id = can_id;
    frame.len = len;  // CAN FDでは .len を使用
    memcpy(frame.data, data, len);

    ssize_t n = write(can_fd, &frame, sizeof(struct canfd_frame));
    if (n < 0) {
        perror("[ERROR] CAN FD write failed");
    }
    return n;
}

// UDS Security Access simulation
#define EXPECTED_KEY 0x5A

void update_security_status(struct canfd_frame *cf, int maxdlen, int can_fd, SecurityContext* ctx) {
  if (cf->len < 2) return;

  Uint8 sid = cf->data[0];
  Uint8 subfn = cf->data[1];
  Uint32 now = SDL_GetTicks();

  struct canfd_frame resp;
  memset(&resp, 0, sizeof(resp));
  resp.can_id = 0x7E8;
  resp.len = 3;

  if (sid != UDS_SECURITY_REQ) return;

  if (subfn == UDS_SECURITY_REQ_SEED) {
    ctx->seed = generate_seed();
    ctx->state = SEC_STATE_WAIT_KEY;
    ctx->seed_sent_time = now;

    resp.data[0] = 0x67;
    resp.data[1] = subfn;
    resp.data[2] = ctx->seed;
    send_can_response(resp.can_id, resp.data, 3, can_fd);
    printf("[UDS] Sent seed: 0x%02X\n", ctx->seed);
  }
  else if (subfn == UDS_SECURITY_REQ_KEY && ctx->state == SEC_STATE_WAIT_KEY) {
    if (now - ctx->seed_sent_time > ctx->timeout_ms) {
      ctx->state = SEC_STATE_IDLE;
      printf("[UDS] Timeout expired\n");
      return;
    }

    if (cf->len < 3) return;

    Uint8 key = cf->data[2];
    Uint8 expected = calculate_key(ctx->seed);

    if (key == expected) {
      car_state.lock_status = OFF;
      car_state.unlock_time = SDL_GetTicks();
      ctx->state = SEC_STATE_IDLE;

      resp.data[0] = 0x67;
      resp.data[1] = subfn;
      resp.data[2] = 0x00;
      send_can_response(resp.can_id, resp.data, 3, can_fd);
      printf("[UDS] Key correct. Unlocked.\n");
    } else {
      car_state.lock_status = ON;
      resp.data[0] = 0x7F;
      resp.data[1] = UDS_SECURITY_REQ;
      resp.data[2] = 0x35;
      send_can_response(resp.can_id, resp.data, 3, can_fd);
      printf("[UDS] Invalid key: 0x%02X (expected: 0x%02X)\n", key, expected);
    }
  }
}


Uint8 generate_seed() {
  return rand() % 256;
}

Uint8 calculate_key(Uint8 seed) {
  return seed ^ 0xAA; // Simple XOR operation for key generation
}


int main(int argc, char *argv[]) {
  setlocale(LC_ALL, "C");
  int opt;
  int can;
  struct ifreq ifr;
  struct sockaddr_can addr;
  struct canfd_frame frame;
  struct iovec iov;
  struct msghdr msg;
  struct cmsghdr *cmsg;
  struct stat dirstat;
  char ctrlmsg[CMSG_SPACE(sizeof(struct timeval)) + CMSG_SPACE(sizeof(__u32))];
  int seed = 0;
  SDL_Event event;

  Uint32 frame_start;
  int frame_time;

  while ((opt = getopt(argc, argv, "rs:dm:h?")) != -1) {
    switch(opt) {
	case 'r':
		randomize = 1;
		break;
	case 's':
		seed = atoi(optarg);
		break;
	case 'd':
		debug = 1;
		break;
	case 'm':
		model = optarg;
		break;
	case 'h':
	case '?':
	default:
		Usage(NULL);
		break;
    }
  }

  if (optind >= argc) Usage("You must specify at least one can device");

  if (seed && randomize) Usage("You can not specify a seed value AND randomize the seed");

  // Verify data directory exists
  if(stat(DATA_DIR, &dirstat) == -1) {
  	printf("ERROR: DATA_DIR not found.  Define in make file or run in src dir\n");
	exit(34);
  }
  
  // Create a new raw CAN socket
  can = socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if(can < 0) Usage("Couldn't create raw socket");

  memset(&ifr.ifr_name, 0, sizeof(ifr.ifr_name));
  strncpy(ifr.ifr_name, argv[optind], strlen(argv[optind]));
  printf("Using CAN interface %s\n", ifr.ifr_name);
  if (ioctl(can, SIOCGIFINDEX, &ifr) < 0) {
    perror("SIOCGIFINDEX");
    exit(1);
  }
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;
  // CAN FD Mode
  setsockopt(can, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &canfd_on, sizeof(canfd_on));

  iov.iov_base = &frame;
  iov.iov_len = sizeof(frame);
  msg.msg_name = &addr;
  msg.msg_namelen = sizeof(addr);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = &ctrlmsg;
  msg.msg_controllen = sizeof(ctrlmsg);
  msg.msg_flags = 0;

  if (bind(can, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
	perror("bind");
	return 1;
  }
  can_thread = SDL_CreateThread(can_receive_thread, "CANThread", &can);

  init_car_state();

  if (randomize || seed) {
	if(randomize) seed = time(NULL);
	srand(seed);
	door_id = (rand() % 2046) + 1;
	signal_id = (rand() % 2046) + 1;
	speed_id = (rand() % 2046) + 1;
	door_pos = rand() % 9;
	signal_pos = rand() % 9;
	speed_pos = rand() % 8;
	printf("Seed: %d\n", seed);
	FILE *fdseed = fopen("/tmp/icsim_seed.txt", "w");
	fprintf(fdseed, "%d\n", seed);
	fclose(fdseed);
  } else if (model) {
	if (!strncmp(model, "bmw", 3)) {
		speed_id = MODEL_BMW_X1_SPEED_ID;
		speed_pos = MODEL_BMW_X1_SPEED_BYTE;
	} else {
		printf("Unknown model.  Acceptable models: bmw\n");
		exit(3);
	}
  }

  SDL_Window *window = NULL;
  if(SDL_Init ( SDL_INIT_VIDEO ) < 0 ) {
	printf("SDL Could not initializes\n");
	exit(40);
  }
  window = SDL_CreateWindow("IC Simulator", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH, SCREEN_HEIGHT,
                            SDL_WINDOW_SHOWN); // | SDL_WINDOW_RESIZABLE);
  if(window == NULL) {
	printf("Window could not be shown\n");
  }
  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  SDL_Surface *image = IMG_Load(get_data("ic.png"));
  SDL_Surface *needle = IMG_Load(get_data("needle.png"));
  SDL_Surface *sprites = IMG_Load(get_data("spritesheet.png"));
  SDL_Surface *lock = IMG_Load(get_data("lock.png"));
  SDL_Surface *unlock = IMG_Load(get_data("unlock.png"));
  base_texture = SDL_CreateTextureFromSurface(renderer, image);
  needle_tex = SDL_CreateTextureFromSurface(renderer, needle);
  sprite_tex = SDL_CreateTextureFromSurface(renderer, sprites);
  lock_tex = SDL_CreateTextureFromSurface(renderer, lock);
  unlock_tex = SDL_CreateTextureFromSurface(renderer, unlock);

  speed_rect.x = 212;
  speed_rect.y = 175;
  speed_rect.h = needle->h;
  speed_rect.w = needle->w;

  // Draw the initial state of the IC
  CarState snapshot = car_state;
  CarState prev_snapshot = car_state;
  redraw_ic(&snapshot, &redraw_flags);
  SDL_RenderPresent(renderer);

  state_mutex = SDL_CreateMutex();

  // 2. Handle drawing and events
  while (running) {
    frame_start = SDL_GetTicks();
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) running = 0;
    }

    SDL_LockMutex(state_mutex);
    snapshot = car_state;
    SDL_UnlockMutex(state_mutex);

    // 3. Check if the state has changed and redraw if necessary
    update_redraw_flags(&prev_snapshot, &snapshot, &redraw_flags);
    if (redraw_flags.speed_redraw || redraw_flags.doors_redraw ||
       redraw_flags.turn_redraw || redraw_flags.lock_redraw) {
      redraw_ic(&snapshot, &redraw_flags);
      SDL_RenderPresent(renderer);
      prev_snapshot = snapshot;
    }

    // 4. Update the lock status if it is ON
    if (snapshot.lock_status == OFF) {
      Uint32 now = SDL_GetTicks();
      if (now - snapshot.unlock_time > 30000) {  // 30秒経過
        SDL_LockMutex(state_mutex);
        car_state.lock_status = ON;
        SDL_UnlockMutex(state_mutex);
        printf("[TIMEOUT] Auto-lock after 30 seconds of inactivity\n");
      }
    }

    // 5. Delay to maintain target FPS
    frame_time = SDL_GetTicks() - frame_start;
    if (frame_time < FRAME_DELAY_MS) {
      SDL_Delay(FRAME_DELAY_MS - frame_time);
    }
  }

  SDL_WaitThread(can_thread, NULL);
  SDL_DestroyMutex(state_mutex);
  SDL_DestroyTexture(base_texture);
  SDL_DestroyTexture(needle_tex);
  SDL_DestroyTexture(sprite_tex);
  SDL_FreeSurface(image);
  SDL_FreeSurface(needle);
  SDL_FreeSurface(sprites);
  SDL_FreeSurface(lock);
  SDL_FreeSurface(unlock);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  IMG_Quit();
  SDL_Quit();

  return 0;
}
