/*
 * Instrument cluster simulator
 *
 * (c) 2014 Open Garages - Craig Smith <craig@theialabs.com>
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

#ifndef DATA_DIR
#define DATA_DIR "./data/"  // Needs trailing slash
#endif

#define SCREEN_WIDTH 692
#define SCREEN_HEIGHT 329
#define DOOR_LOCKED 0
#define DOOR_UNLOCKED 1
#define OFF 0
#define ON 1
#define DEFAULT_DOOR_ID 411 // 0x19b
#define DEFAULT_DOOR_BYTE 2
#define CAN_DOOR1_LOCK 1
#define CAN_DOOR2_LOCK 2 
#define CAN_DOOR3_LOCK 4
#define CAN_DOOR4_LOCK 8
#define DEFAULT_SIGNAL_ID 392 // 0x188
#define DEFAULT_SIGNAL_BYTE 0
#define CAN_LEFT_SIGNAL 1
#define CAN_RIGHT_SIGNAL 2
#define DEFAULT_SPEED_ID 580 // 0x244
#define DEFAULT_SPEED_BYTE 3 // bytes 3,4

// For now, specific models will be done as constants.  Later
// We should use a config file
#define MODEL_BMW_X1_SPEED_ID 0x1B4
#define MODEL_BMW_X1_SPEED_BYTE 0
#define MODEL_BMW_X1_RPM_ID 0x0AA
#define MODEL_BMW_X1_RPM_BYTE 4
#define MODEL_BMW_X1_HANDBRAKE_ID 0x1B4  // Not implemented yet
#define MODEL_BMW_X1_HANDBRAKE_BYTE 5

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
SDL_Rect speed_rect;
SDL_Thread* can_thread = NULL;
SDL_mutex* state_mutex;

typedef struct {
  long speed;
  int door_status[4];
  int turn_status[2];
} CarState;

CarState car_state;


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
  car_state.door_status[0] = DOOR_LOCKED;
  car_state.door_status[1] = DOOR_LOCKED;
  car_state.door_status[2] = DOOR_LOCKED;
  car_state.door_status[3] = DOOR_LOCKED;
  car_state.turn_status[0] = OFF;
  car_state.turn_status[1] = OFF;
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
  if(state->door_status[0] == DOOR_UNLOCKED || state->door_status[1] == DOOR_UNLOCKED ||
     state->door_status[2] == DOOR_UNLOCKED || state->door_status[3] == DOOR_UNLOCKED) {
    // Make the base body red
    update.x = 440;
    update.y = 239;
    update.w = 45;
    update.h = 83;
    memcpy(&pos, &update, sizeof(SDL_Rect));
    pos.x -= 22;
    pos.y -= 22;
    SDL_RenderCopy(renderer, sprite_tex, &update, &pos);
  }

  if(state->door_status[0] == DOOR_UNLOCKED) {
    update.x = 420;
    update.y = 263;
    update.w = 21;
    update.h = 22;
    memcpy(&pos, &update, sizeof(SDL_Rect));
    pos.x -= 22;
    pos.y -= 22;
    SDL_RenderCopy(renderer, sprite_tex, &update, &pos);
  }
  if(state->door_status[1] == DOOR_UNLOCKED) {
    update.x = 484;
    update.y = 261;
    update.w = 21;
    update.h = 22;
    memcpy(&pos, &update, sizeof(SDL_Rect));
    pos.x -= 22;
    pos.y -= 22;
    SDL_RenderCopy(renderer, sprite_tex, &update, &pos);
  }
  if(state->door_status[2] == DOOR_UNLOCKED) {
    update.x = 420;
    update.y = 284;
    update.w = 21;
    update.h = 22;
    memcpy(&pos, &update, sizeof(SDL_Rect));
    pos.x -= 22;
    pos.y -= 22;
    SDL_RenderCopy(renderer, sprite_tex, &update, &pos);
  }
  if(state->door_status[3] == DOOR_UNLOCKED) {
    update.x = 484;
    update.y = 287;
    update.w = 21;
    update.h = 22;
    memcpy(&pos, &update, sizeof(SDL_Rect));
    pos.x -= 22;
    pos.y -= 22;
    SDL_RenderCopy(renderer, sprite_tex, &update, &pos);
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

/* Redraws the IC updating everything */
void redraw_ic(CarState* snapshot) {
  // 1. Clear the screen with the base background texture
  blank_ic();
  
  // 2. Draw all dynamic components on top based on their current state
  update_speed(snapshot);
  update_doors(snapshot);
  update_turn_signals(snapshot);
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
    if (frame.can_id == door_id) update_door_status(&frame, CAN_MTU);
    if (frame.can_id == signal_id) update_signal_status(&frame, CAN_MTU);
    if (frame.can_id == speed_id) update_speed_status(&frame, CAN_MTU);
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
  int nbytes, maxdlen;
  int seed = 0;
  SDL_Event event;

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
  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  SDL_Surface *image = IMG_Load(get_data("ic.png"));
  SDL_Surface *needle = IMG_Load(get_data("needle.png"));
  SDL_Surface *sprites = IMG_Load(get_data("spritesheet.png"));
  base_texture = SDL_CreateTextureFromSurface(renderer, image);
  needle_tex = SDL_CreateTextureFromSurface(renderer, needle);
  sprite_tex = SDL_CreateTextureFromSurface(renderer, sprites);

  speed_rect.x = 212;
  speed_rect.y = 175;
  speed_rect.h = needle->h;
  speed_rect.w = needle->w;

  // Draw the initial state of the IC
  CarState snapshot = car_state;
  redraw_ic(&snapshot);
  SDL_RenderPresent(renderer);

  state_mutex = SDL_CreateMutex();

    // 2. Handle Network Data (non-blocking) to update state
    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = 0;
        }

        SDL_LockMutex(state_mutex);
        snapshot = car_state;
        SDL_UnlockMutex(state_mutex);

        redraw_ic(&snapshot);
        SDL_Delay(16); // 約60FPS
    }

  SDL_WaitThread(can_thread, NULL);
  SDL_DestroyMutex(state_mutex);
  SDL_DestroyTexture(base_texture);
  SDL_DestroyTexture(needle_tex);
  SDL_DestroyTexture(sprite_tex);
  SDL_FreeSurface(image);
  SDL_FreeSurface(needle);
  SDL_FreeSurface(sprites);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  IMG_Quit();
  SDL_Quit();

  return 0;
}
