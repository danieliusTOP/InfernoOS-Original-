#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <sys/syscall.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#define SOCKET_VISUAL "/var/run/snake_visual.socket"
#define SOCKET_APPLAUNCHER "/var/run/snake_applauncher.socket"
#define MAX_APPS 64
#define MAX_MEMORY_PER_APP (64 * 1024 * 1024)
#define IS_LAUNCHER 1
#define NORMAL_APP 0

typedef struct {
    int id;
    char name[64];
    pid_t pid;
    int is_launcher;
} App;

App apps[MAX_APPS];
int app_count = 0;
int visual_fd = -1;

struct sock_filter seccomp_filter[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_read, 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_write, 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_openat, 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_close, 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_exit, 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_exit_group, 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_connect, 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_sendto, 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_recvfrom, 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
};

struct sock_fprog seccomp_prog = {
    .len = sizeof(seccomp_filter) / sizeof(seccomp_filter[0]),
    .filter = seccomp_filter,
};

int connect_to_visual() {
    struct sockaddr_un addr;
    visual_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_VISUAL);
    while (connect(visual_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) sleep(1);
    printf("AppLauncher: Connected to VisualService\n");
    return 0;
}

void register_app(const char *name, int pid) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "REGISTER %s %d 3366FF\n", name, pid);
    write(visual_fd, cmd, strlen(cmd));
}

void limit_resources() {
    struct rlimit limit;
    limit.rlim_cur = MAX_MEMORY_PER_APP;
    limit.rlim_max = MAX_MEMORY_PER_APP;
    setrlimit(RLIMIT_AS, &limit);
    limit.rlim_cur = 50;
    limit.rlim_max = 50;
    setrlimit(RLIMIT_CPU, &limit);
    limit.rlim_cur = 32;
    limit.rlim_max = 32;
    setrlimit(RLIMIT_NOFILE, &limit);
}

void apply_seccomp() {
    prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &seccomp_prog);
}

int launch_lua(const char *path, const char *name, int is_launcher) {
    int app_id = app_count++;
    pid_t pid = fork();
    if (pid == 0) {
        char app_dir[128];
        snprintf(app_dir, sizeof(app_dir), "/data/app_%d", app_id);
        mkdir(app_dir, 0700);
        chdir(app_dir);
        int fd = open("/dev/console", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        limit_resources();
        execl("/bin/lua", "lua", path, NULL);
        exit(1);
    } else if (pid > 0) {
        apps[app_id].id = app_id;
        strcpy(apps[app_id].name, name);
        apps[app_id].pid = pid;
        apps[app_id].is_launcher = is_launcher;
        register_app(name, pid);
        printf("AppLauncher: Launched %s (PID %d) sandbox %d %s\n", 
               name, pid, app_id, is_launcher ? "[LAUNCHER]" : "");
        return app_id;
    }
    return -1;
}

int main() {
    printf("AppLauncher starting...\n");
    connect_to_visual();
    launch_lua("/apps/snakestart.lua", "SnakeStart", IS_LAUNCHER);
    
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    unlink(SOCKET_APPLAUNCHER);
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_APPLAUNCHER);
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);
    printf("AppLauncher: Ready. Launcher is protected.\n");
    
    while (1) {
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd > 0) {
            char buffer[256];
            read(client_fd, buffer, sizeof(buffer));
            if (strncmp(buffer, "SOUND", 5) == 0) {
                char sound_name[128];
                sscanf(buffer, "SOUND %127s", sound_name);
                int voice_fd = socket(AF_UNIX, SOCK_STREAM, 0);
                struct sockaddr_un vaddr;
                vaddr.sun_family = AF_UNIX;
                strcpy(vaddr.sun_path, "/var/run/snake_voice.socket");
                if (connect(voice_fd, (struct sockaddr*)&vaddr, sizeof(vaddr)) == 0) {
                    char cmd[256];
                    snprintf(cmd, sizeof(cmd), "PLAY /system/sounds/%s.wav\n", sound_name);
                    write(voice_fd, cmd, strlen(cmd));
                    close(voice_fd);
                }
            }
            close(client_fd);
        }
        
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            for (int i = 0; i < app_count; i++) {
                if (apps[i].pid == pid) {
                    if (apps[i].is_launcher) {
                        printf("AppLauncher: Launcher died! Restarting...\n");
                        launch_lua("/apps/snakestart.lua", "SnakeStart", IS_LAUNCHER);
                    } else {
                        printf("App %s (PID %d) terminated\n", apps[i].name, pid);
                    }
                    break;
                }
            }
        }
        usleep(10000);
    }
    return 0;
}
