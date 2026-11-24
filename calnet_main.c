#include "calnet_io.h"
#include <stdio.h>
#include <string.h>

#ifdef CALNET_JOIN
bool cal_join(rtl_radio *, struct pipes *, struct cal_station *, const char *);
#endif
static void cancel(int sig) { (void)sig; cal_cancelled = 1; }
struct options { const char *mode, *ssid; };
static bool run(struct pipes *p, void *context) {
    const struct options *options = context;
    const char *mode = options->mode;
    struct rtl_radio_io ops = {.ctx=p->usb, .read=read_reg, .write=write_reg};
    rtl_radio *r = rtl_radio_create(ops);
    if (!r) return false;
    bool ok = rtl_radio_start(r, p->bulk_out_count, 1) == 0;
    struct cal_station station;
    if (!ok) fprintf(stderr, "Initialize: %s\n", rtl_radio_error(r));
    if (ok) ok = cal_discover(r, p, &station,options->ssid);
#ifdef CALNET_JOIN
    if (ok && strcmp(mode, "--discover")) ok = cal_join(r, p, &station, mode);
#else
    (void)mode;
#endif
    if (rtl_radio_stop(r)) { fprintf(stderr,"Shut down: %s\n",rtl_radio_error(r)); ok = false; }
    rtl_radio_destroy(r);
    return ok;
}
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr,"Usage: %s --discover|--associate|--join|--join-saved [exact SSID]\n",argv[0]);
        return 2;
    }
    const char *ssid = argc == 3 ? argv[2] : "CalNet";
    if (!*ssid || strlen(ssid)>32 || strcspn(ssid,"\n\r\t")!=strlen(ssid)) return 2;
    bool join = !strcmp(argv[1], "--join") || !strcmp(argv[1], "--join-saved") ||
                !strcmp(argv[1], "--associate");
#ifndef CALNET_JOIN
    if (join) return 2;
#endif
    if (!join && strcmp(argv[1], "--discover")) return 2;
    setvbuf(stdout,NULL,_IOLBF,0);
    signal(SIGINT,cancel); signal(SIGTERM,cancel);
    struct options options = {argv[1],ssid};
    struct usb_task task = {run,&options};
    return with_identified_adapter(use_interface,&task);
}
