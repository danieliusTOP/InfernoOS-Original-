#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>

#define SOCKET_PATH "/var/run/snake_voice.socket"
#define BUFFER_SIZE 256

int server_fd = -1;
pid_t current_player = 0;

void cleanup(int sig) {
    if (current_player > 0) kill(current_player, SIGTERM);
    if (server_fd >= 0) close(server_fd);
    unlink(SOCKET_PATH);
    exit(0);
}

void set_volume(int percent) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "amixer set Master %d%% > /dev/null 2>&1", percent);
    system(cmd);
    printf("SystemVoice: Volume set to %d%%\n", percent);
}

void stop_playing() {
    if (current_player > 0) {
        kill(current_player, SIGTERM);
        waitpid(current_player, NULL, 0);
        current_player = 0;
    }
}

void play_wav(const char *path) {
    stop_playing();
    
    pid_t pid = fork();
    if (pid == 0) {
        execlp("aplay", "aplay", path, NULL);
        exit(0);
    } else if (pid > 0) {
        current_player = pid;
        printf("SystemVoice: Playing %s (PID %d)\n", path, pid);
    }
}

void beep(int freq_hz, int duration_ms) {
    // Простейший бип через printf("\a") или через /dev/console
    // Но для QEMU лучше использовать aplay сгенерировав синус
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "speaker-test -t sine -f %d -l 1 > /dev/null 2>&1 &", freq_hz);
    system(cmd);
    usleep(duration_ms * 1000);
    system("pkill speaker-test");
    printf("SystemVoice: Beep %d Hz %d ms\n", freq_hz, duration_ms);
}

void handle_command(const char *cmd) {
    char command[BUFFER_SIZE];
    char arg[BUFFER_SIZE];
    
    if (sscanf(cmd, "PLAY %255s", arg) == 1) {
        play_wav(arg);
    }
    else if (sscanf(cmd, "VOLUME %d", &arg[0]) == 1) {
        set_volume(atoi(arg));
    }
    else if (sscanf(cmd, "BEEP %d %d", &arg[0], &arg[1]) == 2) {
        beep(atoi(arg), &arg[1]);
    }
    else if (strcmp(cmd, "STOP") == 0) {
        stop_playing();
    }
    else if (strcmp(cmd, "STATUS") == 0) {
        printf("SystemVoice: Current PID: %d\n", current_player);
    }
    else {
        printf("SystemVoice: Unknown command: %s\n", cmd);
    }
}

int main() {
    printf("SystemVoice starting...\n");
    
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);
    
    // Создаём сокет
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
    
    struct sockaddr_un addr;
    unlink(SOCKET_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }
    
    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }
    
    printf("SystemVoice ready on %s\n", SOCKET_PATH);
    printf("Commands: PLAY <file.wav>, VOLUME <0-100>, BEEP <freq> <ms>, STOP, STATUS\n");
    
    char buffer[BUFFER_SIZE];
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }
        
        memset(buffer, 0, sizeof(buffer));
        ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            buffer[strcspn(buffer, "\r\n")] = '\0';
            handle_command(buffer);
        }
        close(client_fd);
    }
    
    return 0;
}
