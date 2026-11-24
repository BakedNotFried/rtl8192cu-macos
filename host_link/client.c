#include "client.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <net/if.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
enum { max_frame = 1514 };
struct host_connection {
    int socket;
    pid_t launcher;
    bool cleanup_failed;
    char awaiting_ipv4[IFNAMSIZ];
    uint64_t next_address_check;
    uint8_t incoming[max_frame + 2];
    size_t used;
};

// Only argv values enter the shell, individually quoted by AppleScript. macOS
// owns the administrator prompt. The USB/Keychain process stays unprivileged.
static const char script[] =
    "on run a\n"
    "do shell script (quoted form of (item 1 of a) & \" \" & quoted form of (item 2 of a) & "
    "\" --session \" & quoted form of (item 3 of a) & \" \" & quoted form of (item 4 of a)) "
    "with administrator privileges\nend run";

static int make_listener(char *helper, size_t capacity, struct sockaddr_un *address) {
    uint32_t size = PATH_MAX;
    char executable[PATH_MAX], absolute[PATH_MAX], directory[PATH_MAX];
    if (_NSGetExecutablePath(executable,&size) || !realpath(executable,absolute)) return -1;
    char *base = strrchr(absolute,'/');
    if (!base) return -1;
    *base = 0;
    if (snprintf(helper,capacity,"%s/host-link-helper",absolute) >= (int)capacity ||
        access(helper,X_OK) || snprintf(directory,sizeof(directory),"%s/session.XXXXXX",absolute) >= (int)sizeof(directory))
        return -1;
    if (strlen(directory)+3 >= sizeof(address->sun_path)) {
        fprintf(stderr,"Build path too long for the macOS local socket; use a shorter checkout path.\n");
        return -1;
    }
    if (!mkdtemp(directory)) return -1;
    address->sun_family = AF_UNIX;
    snprintf(address->sun_path,sizeof(address->sun_path),"%s/s",directory);
    int fd = socket(AF_UNIX,SOCK_STREAM,0);
    if (fd < 0) return -1;
    if (fcntl(fd,F_SETFD,FD_CLOEXEC) ||
        bind(fd,(struct sockaddr *)address,sizeof(*address)) || listen(fd,1)) {
        close(fd);
        return -1;
    }
    return fd;
}

static pid_t wait_launcher(pid_t child, int *status, int options) {
    pid_t result;
    do { result = waitpid(child,status,options); } while (result < 0 && errno == EINTR);
    return result;
}

static int spawn_launcher(pid_t *child, const char *path, char *const args[]) {
    posix_spawnattr_t attr;
    int error = posix_spawnattr_init(&attr);
    if (error) return error;
    // A terminal interrupt belongs to the station. The administrator launcher
    // must remain alive until its supervisor has removed the temporary link.
    error = posix_spawnattr_setpgroup(&attr,0);
    if (!error) error = posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP);
    if (!error) error = posix_spawn(child,path,NULL,&attr,args,environ);
    posix_spawnattr_destroy(&attr);
    return error;
}

static void close_stream(struct host_connection *c) {
    // Deliver EOF after the final frame. The worker can finish an in-flight
    // write before it sees EOF and exits. Its socket timeout is three seconds.
    if (shutdown(c->socket,SHUT_WR) == 0 && c->launcher > 0) {
        uint8_t discard[4096];
        uint64_t until = cal_millis()+3500;
        while (cal_millis()<until) {
            struct pollfd p = {c->socket,POLLIN,0};
            int ready = poll(&p,1,200);
            if (!ready || (ready < 0 && errno == EINTR)) continue;
            if (ready < 0) break;
            ssize_t n = recv(c->socket,discard,sizeof(discard),MSG_DONTWAIT);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (n <= 0) break;
        }
    }
    close(c->socket);
    c->socket = -1;
}

static void stop_host(void *ctx) {
    struct host_connection *c = ctx;
    bool connected = c->socket >= 0;
    if (connected) close_stream(c);
    if (c->launcher <= 0) return;
    int status = 0;
    pid_t w = wait_launcher(c->launcher,&status,WNOHANG);
    if (w == 0) {
        // Before connect there is no active session. Dismiss only our
        // unprivileged launcher. A late helper cannot reach the closed listener.
        if (!connected) kill(c->launcher,SIGTERM);
        uint64_t until = cal_millis()+(connected ? 8000 : 750);
        while (w == 0 && cal_millis()<until) {
            usleep(25000);
            w = wait_launcher(c->launcher,&status,WNOHANG);
        }
        if (w == 0) {
            if (connected)
                fputs("Waiting for the temporary interface to finish shutting down.\n",stderr);
            else
                kill(c->launcher,SIGKILL);
            // After connect the privileged supervisor owns cleanup. Its stream
            // is closed, but keep its launcher alive to report teardown status.
            w = wait_launcher(c->launcher,&status,0);
        }
    }
    if (connected && (w < 0 || !WIFEXITED(status) || WEXITSTATUS(status))) {
        c->cleanup_failed=true;
        fputs("Administrator helper cleanup could not be confirmed.\n",stderr);
    }
    c->launcher=0;
}

static int read_exact(int fd, uint8_t *bytes, size_t n, uint64_t until) {
    while (n && !cal_cancelled && cal_millis()<until) {
        struct pollfd p = {fd,POLLIN,0};
        int ready = poll(&p,1,200);
        if (ready < 0 && errno != EINTR) return -1;
        if (ready <= 0) continue;
        ssize_t k = recv(fd,bytes,n,0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        bytes += k; n -= (size_t)k;
    }
    return n ? -1 : 0;
}

static int prepare_session_socket(int fd) {
    int yes = 1;
    if (fcntl(fd,F_SETFD,FD_CLOEXEC) < 0) return -1;
    return setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes));
}

static int start_host(void *ctx,const uint8_t mac[6]) {
    struct host_connection *c = ctx;
    char helper[PATH_MAX], text[18], uid[32];
    struct sockaddr_un address = {0};
    int listener = make_listener(helper,sizeof(helper),&address);
    if (listener < 0) { perror("Host link setup"); return -1; }
    snprintf(text,sizeof(text),"%02x:%02x:%02x:%02x:%02x:%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    snprintf(uid,sizeof(uid),"%u",(unsigned)getuid());
    char *args[] = {"osascript","-e",(char *)script,helper,text,address.sun_path,uid,NULL};
    puts("Waiting for macOS administrator approval for the temporary Ethernet link.");
    pid_t spawned;
    int e = spawn_launcher(&spawned,"/usr/bin/osascript",args);
    if (e) {
        close(listener);
        fprintf(stderr,"Could not start administrator prompt: %s\n",strerror(e));
        return -1;
    }
    c->launcher=spawned;
    uint64_t until = cal_millis()+120000;
    while (!cal_cancelled && cal_millis()<until) {
        pid_t w = wait_launcher(c->launcher,NULL,WNOHANG);
        if (w != 0) { c->launcher=0; break; }
        struct pollfd p = {listener,POLLIN,0};
        int ready = poll(&p,1,200);
        if (ready < 0 && errno != EINTR) break;
        if (ready <= 0) continue;
        int candidate = accept(listener,NULL,NULL);
        if (candidate < 0 && errno == EINTR) continue;
        if (candidate < 0) break;
        uid_t peer; gid_t group;
        if (!getpeereid(candidate,&peer,&group) && peer == 0) { c->socket=candidate; break; }
        close(candidate);
    }
    close(listener);
    if (c->socket >= 0) {
        uint8_t header[2]; char name[IFNAMSIZ]={0};
        until = cal_millis()+20000;
        if (!prepare_session_socket(c->socket) &&
            !read_exact(c->socket,header,2,until) && !header[0] && header[1]>4 &&
            header[1]<sizeof(name) && !read_exact(c->socket,(uint8_t *)name,header[1],until) &&
            !strncmp(name,"feth",4) && strspn(name+4,"0123456789")== (size_t)header[1]-4 &&
            fcntl(c->socket,F_SETFL,O_NONBLOCK) == 0) {
            strlcpy(c->awaiting_ipv4,name,sizeof(c->awaiting_ipv4));
            printf("Host interface: %s (macOS DHCP started)\n",name);
            return 0;
        }
    }
    fprintf(stderr,"Temporary host interface did not activate.\n");
    stop_host(c);
    return -1;
}

static bool assigned_ipv4(const char *name, const struct ifaddrs *list, char address[INET_ADDRSTRLEN]) {
    for (const struct ifaddrs *ifp=list; ifp; ifp=ifp->ifa_next) {
        if (!ifp->ifa_addr || ifp->ifa_addr->sa_family!=AF_INET || strcmp(ifp->ifa_name,name)) continue;
        const struct sockaddr_in *v4=(const struct sockaddr_in *)ifp->ifa_addr;
        uint32_t ip=ntohl(v4->sin_addr.s_addr);
        if (ip && (ip >> 16)!=0xa9fe && inet_ntop(AF_INET,&v4->sin_addr,address,INET_ADDRSTRLEN)) return true;
    }
    return false;
}

static void report_ipv4(struct host_connection *c) {
    if (!c->awaiting_ipv4[0]) return;
    uint64_t now=cal_millis();
    if (now<c->next_address_check) return;
    c->next_address_check=now+1000;
    struct ifaddrs *list;
    if (getifaddrs(&list)!=0) return;
    char address[INET_ADDRSTRLEN];
    if (assigned_ipv4(c->awaiting_ipv4,list,address)) {
        printf("Host link ready on %s with IPv4 address %s\n",c->awaiting_ipv4,address);
        fflush(stdout);
        c->awaiting_ipv4[0]=0;
    }
    freeifaddrs(list);
}

static int host_to_radio(void *ctx,uint8_t *out,size_t capacity,size_t *length) {
    struct host_connection *c = ctx;
    report_ipv4(c);
    for (;;) {
        size_t need = 2;
        if (c->used>=2) {
            size_t payload = (size_t)c->incoming[0]*256+c->incoming[1];
            if (payload<14 || payload>max_frame || payload>capacity) return -1;
            need += payload;
            if (c->used==need) {
                memcpy(out,c->incoming+2,payload); *length=payload; c->used=0;
                return 1;
            }
        }
        ssize_t n = recv(c->socket,c->incoming+c->used,need-c->used,0);
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0;
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return -1;
        c->used+=(size_t)n;
    }
}
static int radio_to_host(void *ctx,const uint8_t *packet,size_t n) {
    struct host_connection *c=ctx;
    if (n<14 || n>max_frame) return -1;
    if (cal_cancelled || !(packet[12]==8 && (packet[13]==0 || packet[13]==6))) return 0;
    uint8_t record[max_frame+2]={(uint8_t)(n>>8),(uint8_t)n};
    memcpy(record+2,packet,n);
    size_t sent=0;
    uint64_t until=cal_millis()+1000;
    // Complete a started record before signaling EOF, even when interrupted.
    while (sent<n+2 && cal_millis()<until) {
        ssize_t k=send(c->socket,record+sent,n+2-sent,0);
        if (k>0) {sent+=(size_t)k;continue;}
        if (k<0 && errno==EINTR) continue;
        if (k<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {
            struct pollfd p={c->socket,POLLOUT,0}; poll(&p,1,50); continue;
        }
        return -1;
    }
    return sent==n+2 ? 0 : -1;
}

bool run_host_connection(rtl_radio *r,struct pipes *usb,struct cal_station *s,bool saved) {
    if (getuid()==0 || geteuid()!=getuid()) {
        fprintf(stderr,"Run the station as your regular macOS user.\n");
        return false;
    }
    struct host_connection c = {.socket=-1};
    struct cal_link_ops ops = {.ctx=&c,.up=start_host,.down=stop_host,
                               .read_frame=host_to_radio,.write_frame=radio_to_host};
    return cal_join_persistent(r,usb,s,saved,&ops) && !c.cleanup_failed;
}
