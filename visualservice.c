#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdarg.h>
#include <sys/stat.h>

void play_sound(const char *sound_name) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return;
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/var/run/snake_voice.socket");
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "PLAY %s\n", sound_name);
        write(sock, cmd, strlen(cmd));
    }
    close(sock);
}

#define SOCKET_VISUAL "/var/run/snake_visual.socket"
#define SOCKET_ACTOR "/var/run/snake_actor.socket"
#define SOCKET_APPLAUNCHER "/var/run/snake_app_events.socket"
#define MAX_WINDOWS 16

typedef struct {
    int id;
    int x, y, w, h;
    int app_id;
} VisualWindow;

VisualWindow windows[MAX_WINDOWS];
int window_count = 0;
int screen_width = 640;
int screen_height = 480;
int actor_fd = -1;
int applauncher_fd = -1;

int connect_to_actor() {
    struct sockaddr_un addr;
    actor_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_ACTOR);
    while (connect(actor_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) sleep(1);
    printf("VisualService: Connected to Actor\n");
    return 0;
}

int connect_to_applauncher() {
    struct sockaddr_un addr;
    applauncher_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_APPLAUNCHER);
    while (connect(applauncher_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) sleep(1);
    printf("VisualService: Connected to AppLauncher\n");
    return 0;
}

void send_to_applauncher(const char *fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    strcat(buffer, "\n");
    write(applauncher_fd, buffer, strlen(buffer));
}

void add_window(int id, int x, int y, int w, int h, int app_id) {
    if (window_count < MAX_WINDOWS) {
        windows[window_count].id = id;
        windows[window_count].x = x;
        windows[window_count].y = y;
        windows[window_count].w = w;
        windows[window_count].h = h;
        windows[window_count].app_id = app_id;
        window_count++;
        printf("VisualService: Added window %d for app %d at (%d,%d) %dx%d\n", 
               id, app_id, x, y, w, h);
    }
}

int get_window_at(int x, int y) {
    for (int i = window_count-1; i >= 0; i--) {
        if (x >= windows[i].x && x < windows[i].x + windows[i].w &&
            y >= windows[i].y && y < windows[i].y + windows[i].h) {
            return windows[i].app_id;
        }
    }
    return -1;
}

void handle_command(char *cmd) {
    printf("VisualService received: %s\n", cmd);
    
    int id, x, y, w, h, app_id, pressed;
    char name[64];
    
    if (strncmp(cmd, "REGISTER", 8) == 0) {
        sscanf(cmd, "REGISTER %s %d %x", name, &id, &id);
        printf("VisualService: Registered app %s (ID %d)\n", name, id);
    }
    else if (strncmp(cmd, "WINDOW", 6) == 0) {
        sscanf(cmd, "WINDOW %d %d %d %d %d %d", &id, &x, &y, &w, &h, &app_id);
        add_window(id, x, y, w, h, app_id);
        
        // ОТПРАВКА В ACTOR ОТКЛЮЧЕНА — Actor 1.0 не понимает WINDOW_CREATE
        // Команды PNG/JPEG/TEXT будут идти напрямую от SnakeStart в Actor
        printf("VisualService: Window added (not sent to Actor, Actor 1.0 mode)\n");
    }
    else if (strncmp(cmd, "GESTURE", 7) == 0) {
        int gesture, x, y, pressed;
        sscanf(cmd, "GESTURE %d %d %d %d", &gesture, &x, &y, &pressed);
        int app_id = get_window_at(x, y);
        if (app_id >= 0) {
            send_to_applauncher("EVENT %d GESTURE %d %d %d %d", app_id, gesture, x, y, pressed);
        }
    }
    else if (strncmp(cmd, "TOUCH", 5) == 0) {
        sscanf(cmd, "TOUCH %d %d %d", &x, &y, &pressed);
        int app_id = get_window_at(x, y);
        if (app_id >= 0) {
            printf("VisualService: Touch at (%d,%d) -> app %d\n", x, y, app_id);
            send_to_applauncher("EVENT %d TOUCH %d %d %d", app_id, x, y, pressed);
        
        // ЗВУК ПРИ КАСАНИИ
        if (pressed) {
            play_sound("/system/sounds/tap.wav");
        }
        } else {
            printf("VisualService: Touch at (%d,%d) -> no window\n", x, y);
        }
    }
}

int main() {
    mkdir("/var/run/snake", 0755);
    printf("VisualService starting...\n");
    
    connect_to_actor();
    // write(actor_fd, "TEST", 4);
    connect_to_applauncher();
    
    // Добавляем тестовое окно лаунчера (для внутренней таблицы VisualService)
    add_window(0, 0, 0, screen_width, screen_height, 0);
    
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    
    // Удаляем старый сокет, если он существует
    unlink(SOCKET_VISUAL);
    
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_VISUAL);
    
    // Проверяем bind
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    
    // Проверяем listen
    if (listen(server_fd, 5) < 0) {
        perror("listen");
        return 1;
    }
    
    printf("VisualService: Ready on %s\n", SOCKET_VISUAL);
    printf("VisualService: Waiting for commands... (Actor 1.0 mode, no window commands forwarded)\n");
    
    while (1) {
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd > 0) {
            char buffer[256];
            memset(buffer, 0, sizeof(buffer));
            read(client_fd, buffer, sizeof(buffer));
            buffer[strcspn(buffer, "\r\n")] = 0;
            handle_command(buffer);
            close(client_fd);
        }
        usleep(10000);
    }
    
    return 0;
}
