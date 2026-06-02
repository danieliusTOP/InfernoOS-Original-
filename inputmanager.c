#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <time.h>

#define MAX_FINGERS 10
#define SOCKET_VISUAL "/var/run/snake_visual.socket"
#define SWIPE_THRESHOLD 50
#define HOLD_THRESHOLD 800

#define GESTURE_NONE 0
#define GESTURE_TAP 1
#define GESTURE_HOLD 2
#define GESTURE_SWIPE_UP 3
#define GESTURE_SWIPE_DOWN 4
#define GESTURE_SWIPE_LEFT 5
#define GESTURE_SWIPE_RIGHT 6

typedef struct {
    int x, y;
    unsigned long start_time;
    int pressed;
    int moved;
} Gesture;

typedef struct {
    int fd;
    char name[256];
    int is_touch;
    int is_mouse;
    int is_keyboard;
} InputDevice;

InputDevice devices[16];
int device_count = 0;
Gesture gestures[MAX_FINGERS];
int visual_fd = -1;

unsigned long get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int get_gesture_type(Gesture *g, int x, int y, unsigned long time) {
    int dx = x - g->x;
    int dy = y - g->y;
    int dt = time - g->start_time;
    
    if (!g->pressed) {
        if (dt < HOLD_THRESHOLD && !g->moved) {
            return GESTURE_TAP;
        }
        return GESTURE_NONE;
    }
    
    if (dt > HOLD_THRESHOLD) {
        return GESTURE_HOLD;
    }
    
    if (abs(dx) > SWIPE_THRESHOLD || abs(dy) > SWIPE_THRESHOLD) {
        g->moved = 1;
        if (abs(dx) > abs(dy)) {
            return dx > 0 ? GESTURE_SWIPE_RIGHT : GESTURE_SWIPE_LEFT;
        } else {
            return dy > 0 ? GESTURE_SWIPE_DOWN : GESTURE_SWIPE_UP;
        }
    }
    
    return GESTURE_NONE;
}

void scan_input_devices() {
    DIR *dir = opendir("/dev/input");
    if (!dir) return;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        
        char path[256];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        
        char name[256] = "Unknown";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        
        devices[device_count].fd = fd;
        strcpy(devices[device_count].name, name);
        
        devices[device_count].is_touch = (strstr(name, "Touch") != NULL) ||
                                          (strstr(name, "touch") != NULL);
        devices[device_count].is_mouse = (strstr(name, "Mouse") != NULL) ||
                                          (strstr(name, "mouse") != NULL);
        devices[device_count].is_keyboard = (strstr(name, "Keyboard") != NULL) ||
                                            (strstr(name, "keyboard") != NULL);
        
        printf("InputManager: Found %s: %s\n", entry->d_name, name);
        device_count++;
    }
    closedir(dir);
}

int connect_to_visual() {
    struct sockaddr_un addr;
    
    // Ждём появления файла сокета на диске
    int attempts = 0;
    int max_attempts = 1000;
    while (access(SOCKET_VISUAL, F_OK) == -1) {
        if (attempts++ > max_attempts) { // Ждём ~5 секунд (50 * 0.1)
            printf("InputManager: Timeout waiting for socket file %s\n", SOCKET_VISUAL);
            return -1;
        }
        usleep(100000); // 0.1 секунды // 0.1 секунды
    }
    
    // Файл появился, создаём сокет
    visual_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (visual_fd < 0) return -1;
    
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_VISUAL);
    
    // Пытаемся подключиться
    attempts = 0;
    while (connect(visual_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (attempts++ > max_attempts) {
            printf("InputManager: Failed to connect to VisualService\n");
            close(visual_fd);
            visual_fd = -1;
            return -1;
        }
        usleep(100000); // 0.1 секунды
    }
    
    printf("InputManager: Connected to VisualService\n");
    return 0;
}

void send_gesture(int gesture, int x, int y, int pressed) {
    if (visual_fd < 0) return;
    
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "GESTURE %d %d %d %d\n", gesture, x, y, pressed);
    write(visual_fd, buffer, strlen(buffer));
    printf("InputManager: Sent gesture %d at (%d,%d) pressed=%d\n", gesture, x, y, pressed);
}

int main() {
    printf("InputManager starting...\n");
    
    scan_input_devices();
    
    if (device_count == 0) {
        printf("InputManager: No input devices found!\n");
        return 1;
    }
    
    if (connect_to_visual() < 0) {
        printf("InputManager: Cannot continue without VisualService\n");
        return 1;
    }
    
    printf("InputManager started. Monitoring %d devices with gesture support...\n", device_count);
    
    struct input_event ev;
    int mouse_x = 0, mouse_y = 0;
    unsigned long last_time = 0;
    
    while (1) {
        for (int i = 0; i < device_count; i++) {
            int bytes = read(devices[i].fd, &ev, sizeof(ev));
            
            if (bytes == sizeof(ev)) {
                if (ev.type == EV_SYN) continue;
                
                if (devices[i].is_mouse) {
                    if (ev.type == EV_REL) {
                        if (ev.code == REL_X) mouse_x += ev.value;
                        if (ev.code == REL_Y) mouse_y += ev.value;
                    }
                    if (ev.type == EV_KEY && ev.code == BTN_LEFT) {
                        unsigned long now = get_time_ms();
                        
                        if (ev.value) {
                            gestures[0].x = mouse_x;
                            gestures[0].y = mouse_y;
                            gestures[0].start_time = now;
                            gestures[0].pressed = 1;
                            gestures[0].moved = 0;
                        } else {
                            gestures[0].pressed = 0;
                            int gesture = get_gesture_type(&gestures[0], mouse_x, mouse_y, now);
                            send_gesture(gesture, mouse_x, mouse_y, 0);
                        }
                    }
                }
            }
        }
        
        usleep(1000);
    }
    
    return 0;
}
