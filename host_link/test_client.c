// Offline IPC checks: local socketpairs only, never start the administrator helper.
#include "client.c"
#include "admin_socket.h"
#include <assert.h>
#include <time.h>

volatile sig_atomic_t cal_cancelled;
uint64_t cal_millis(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000; }
bool cal_join_persistent(rtl_radio *r, struct pipes *p, struct cal_station *s,
                         bool saved, const struct cal_link_ops *link) {
    (void)r; (void)p; (void)s; (void)saved; (void)link;
    abort();
}

static void stream_test(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(prepare_session_socket(pair[0]) == 0);
    assert(fcntl(pair[0], F_GETFD) & FD_CLOEXEC);
    int no_sigpipe = 0;
    socklen_t option_size = sizeof(no_sigpipe);
    assert(getsockopt(pair[0],SOL_SOCKET,SO_NOSIGPIPE,&no_sigpipe,&option_size) == 0 && no_sigpipe);
    assert(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
    struct host_connection c = {.socket = pair[0]};
    uint8_t received[1514];
    size_t length = 0;
    const uint8_t arp[14] = {0xff,0xff,0xff,0xff,0xff,0xff, 2,0,0,0,0,1, 8,6};
    const uint8_t prefix[] = {0,sizeof(arp)};
    assert(host_to_radio(&c,received,sizeof(received),&length) == 0);
    assert(send(pair[1],prefix,1,0) == 1);
    assert(host_to_radio(&c,received,sizeof(received),&length) == 0);
    assert(send(pair[1],prefix+1,1,0) == 1);
    assert(send(pair[1],arp,3,0) == 3);
    assert(host_to_radio(&c,received,sizeof(received),&length) == 0);
    assert(send(pair[1],arp+3,sizeof(arp)-3,0) == (ssize_t)sizeof(arp)-3);
    assert(send(pair[1],prefix,2,0)==2 && send(pair[1],arp,sizeof(arp),0)==sizeof(arp));
    for (int i=0;i<2;++i) {
        assert(host_to_radio(&c,received,sizeof(received),&length) == 1);
        assert(length == sizeof(arp) && !memcmp(received,arp,length));
    }
    uint8_t other[14] = {0};
    other[12]=0x86; other[13]=0xdd;
    assert(radio_to_host(&c,other,sizeof(other)) == 0);
    assert(recv(pair[1],received,1,MSG_DONTWAIT)==-1 && (errno==EAGAIN || errno==EWOULDBLOCK));
    assert(radio_to_host(&c,arp,sizeof(arp)) == 0);
    assert(recv(pair[1],received,2+sizeof(arp),MSG_WAITALL)==2+sizeof(arp));
    assert(!memcmp(received,prefix,2) && !memcmp(received+2,arp,sizeof(arp)));
    cal_cancelled=1;
    assert(radio_to_host(&c,arp,sizeof(arp)) == 0);
    assert(recv(pair[1],received,1,MSG_DONTWAIT)==-1 && (errno==EAGAIN || errno==EWOULDBLOCK));
    assert(read_exact(c.socket,received,1,cal_millis()+100) != 0);
    cal_cancelled=0;
    const uint8_t too_long[]={0x06,0x00};
    assert(send(pair[1],too_long,2,0)==2);
    assert(host_to_radio(&c,received,sizeof(received),&length) == -1);
    stop_host(&c);
    assert(c.socket==-1 && recv(pair[1],received,sizeof(received),0)==0);
    close(pair[1]);
}

static void reaps_launcher_on_stream_close(int exit_status) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair)==0);
    pid_t child=fork();
    assert(child>=0);
    if (!child) {
        close(pair[0]);
        uint8_t ignored;
        _exit(recv(pair[1],&ignored,1,0)==0 ? exit_status : 1);
    }
    close(pair[1]);
    struct host_connection c={.socket=pair[0],.launcher=child};
    stop_host(&c);
    assert(c.socket==-1 && c.launcher==0);
    assert(c.cleanup_failed == (exit_status != 0));
    assert(waitpid(child,NULL,WNOHANG)==-1 && errno==ECHILD);
    stop_host(&c); // idempotent and no unrelated descriptors or children affected
}

static void reaps_pending_prompt(bool ignores_term) {
    int ready[2];
    assert(pipe(ready)==0);
    pid_t child=fork();
    assert(child>=0);
    if (!child) {
        close(ready[0]);
        signal(SIGTERM,ignores_term ? SIG_IGN : SIG_DFL);
        if (write(ready[1],"x",1)!=1) _exit(1);
        close(ready[1]);
        for (;;) pause();
    }
    close(ready[1]);
    char marker;
    assert(read(ready[0],&marker,1)==1);
    close(ready[0]);
    struct host_connection c={.socket=-1,.launcher=child};
    cal_cancelled=1;
    stop_host(&c);
    cal_cancelled=0;
    assert(c.launcher==0 && !c.cleanup_failed);
    assert(waitpid(child,NULL,WNOHANG)==-1 && errno==ECHILD);
}

static void address_test(void) {
    struct sockaddr_in link_local={.sin_family=AF_INET}, lease={.sin_family=AF_INET};
    struct ifaddrs other={.ifa_name="feth2",.ifa_addr=(struct sockaddr *)&lease};
    struct ifaddrs local={.ifa_name="feth1",.ifa_addr=(struct sockaddr *)&link_local,.ifa_next=&other};
    assert(inet_pton(AF_INET,"192.0.2.5",&lease.sin_addr)==1);
    char address[INET_ADDRSTRLEN];
    assert(!assigned_ipv4("feth1",&local,address));
    assert(inet_pton(AF_INET,"169.254.11.21",&link_local.sin_addr)==1);
    assert(!assigned_ipv4("feth1",&local,address));
    other.ifa_name="feth1";
    assert(assigned_ipv4("feth1",&local,address) && !strcmp(address,"192.0.2.5"));
}

static void cancel_fixture(int signo) {
    (void)signo;
    cal_cancelled=1;
}

static int launch_fixture(int fd) {
    if (getpid()!=getpgrp() || signal(SIGINT,SIG_DFL)==SIG_ERR) return 1;
    if (write(fd,"R",1)!=1) return 1;
    char marker;
    if (read(fd,&marker,1)!=0) return 1;
    // Model a worker finishing buffered output after the client's write EOF.
    uint8_t bytes[4096]={0};
    for (unsigned i=0; i<64; ++i) {
        if (write(fd,bytes,sizeof(bytes))!=(ssize_t)sizeof(bytes)) return 1;
    }
    return 0;
}

static void isolates_terminal_interrupt(char *executable) {
    pid_t station = fork();
    assert(station >= 0);
    if (!station) {
        // SIGINT goes only to this new fixture process group.
        assert(setpgid(0,0)==0 && getpgrp()==getpid());
        assert(signal(SIGINT,cancel_fixture)!=SIG_ERR);
        int pair[2];
        assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
        assert(prepare_session_socket(pair[0])==0);
        char fd_text[32];
        snprintf(fd_text,sizeof(fd_text),"%d",pair[1]);
        char *args[]={executable,"--launcher-fixture",fd_text,NULL};
        pid_t launcher;
        assert(spawn_launcher(&launcher,executable,args)==0);
        close(pair[1]);
        char ready;
        assert(read(pair[0],&ready,1)==1 && ready=='R');
        assert(getpgid(launcher)==launcher && launcher!=getpgrp());
        assert(killpg(getpgrp(),SIGINT)==0 && cal_cancelled);
        struct host_connection c={.socket=pair[0],.launcher=launcher};
        stop_host(&c);
        assert(!c.cleanup_failed && c.socket==-1 && c.launcher==0);
        _exit(0);
    }
    int status;
    assert(wait_launcher(station,&status,0)==station && WIFEXITED(status) && WEXITSTATUS(status)==0);
}

int main(int argc,char **argv) {
    if (argc==3 && !strcmp(argv[1],"--launcher-fixture")) return launch_fixture(atoi(argv[2]));
    assert(host_link_connect_session("relative", "501") == -1);
    assert(host_link_connect_session("/unused", "0") == -1);
    assert(host_link_connect_session("/unused", "501oops") == -1);
    stream_test();
    reaps_launcher_on_stream_close(0);
    reaps_launcher_on_stream_close(1);
    reaps_pending_prompt(false);
    reaps_pending_prompt(true);
    isolates_terminal_interrupt(argv[0]);
    address_test();
    puts("Private client IPC framing, isolated SIGINT, graceful EOF, cancellation and child cleanup passed");
}
