/* switch2-bt.c — Nintendo Switch 2 Pro Controller BLE-to-UHID Daemon
 *
 * Uses BlueZ D-Bus + UHID. Protocol reference: ndeadly/switch2_controller_research
 * Build: aarch64-linux-gnu-gcc -O2 -Wall -o switch2-bt switch2-bt.c \
 *        $(pkg-config --cflags --libs libsystemd)
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <linux/uhid.h>
#include <systemd/sd-bus.h>

#include "hid_report_descriptor.h"

/* ─── Logging ─── */
static int g_verbose;
#define LOG(fmt,...) fprintf(stderr, "[switch2-bt] " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt,...)  fprintf(stderr, "[switch2-bt] ERROR: " fmt "\n", ##__VA_ARGS__)
#define DBG(fmt,...)  do{if(g_verbose)fprintf(stderr,"[switch2-bt] DBG: " fmt "\n",##__VA_ARGS__);}while(0)

/* ─── BlueZ D-Bus Constants ─── */
#define BLUEZ           "org.bluez"
#define IFACE_ADAPTER   "org.bluez.Adapter1"
#define IFACE_DEVICE    "org.bluez.Device1"
#define IFACE_GATTSVC   "org.bluez.GattService1"
#define IFACE_GATTCHAR  "org.bluez.GattCharacteristic1"
#define IFACE_GATTDESC  "org.bluez.GattDescriptor1"
#define IFACE_OBJMGR    "org.freedesktop.DBus.ObjectManager"
#define IFACE_PROPS     "org.freedesktop.DBus.Properties"

/* Nintendo vendor for discovery */
#define VID_NINTENDO    0x057E
#define PID_SWITCH2_MIN 0x2060
#define PID_SWITCH2_MAX 0x206F

/* ─── GATT UUIDs — ndeadly/switch2_controller_research ─── */
#define UUID_SVC_NWCP     "ab7de9be-89fe-49ad-828f-118f09df7fd0"

/* Pro Controller characteristics */
#define UUID_CH_INPUT     "7492866c-ec3e-4619-8258-32755ffcc0f8"   /* 0x000E */
#define UUID_CH_OUTPUT    "cc483f51-9258-427d-a939-630c31f72b05"   /* 0x0012 */
#define UUID_CH_CMD_OUT   "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005"   /* 0x0014 */
#define UUID_CH_CMD_DATA  "3dacbc7e-6955-40b5-8eaf-6f9809e8b379"   /* 0x0016 */
#define UUID_CH_RESP1     "c765a961-d9d8-4d36-a20a-5315b111836a"    /* 0x001A */
#define UUID_CH_RESP2     "506d9f7d-4278-4e95-a549-326ba77657e0"    /* 0x001E */
#define UUID_CH_WRITE5    "00c5af5d-1964-4e30-8f51-1956f96bd282"   /* 0x0005 */
#define UUID_CH_RATEGO    "679d5510-5a24-4dee-9557-95df80486ecb"   /* report rate desc */
#define UUID_CH_CCC       "00002902-0000-1000-8000-00805f9b34fb"

/* ─── NWCP Protocol ─── */
#define NWCP_HDR_LEN      8
#define NWCP_DIR_OUT      0x91
#define NWCP_TRANS_BT     0x01

#define CMD_INIT_HANDSHAKE 0x07
#define CMD_FLASH_READ     0x02
#define CMD_FEATSEL        0x0C
#define CMD_LED            0x09
#define CMD_VIBRATE        0x0A
#define CMD_UNKNOWN_16     0x16
#define CMD_UNKNOWN_11     0x11

#define SUB_FEAT_SET_MASK  0x02
#define SUB_FEAT_ENABLE    0x04
#define SUB_LED_PATTERN    0x07

/* ─── GATT Handles ─── */
typedef struct {
    char path_input[128];
    char path_ccc_input[128];
    char path_output[128];
    char path_cmd_out[128];
    char path_cmd_data[128];
    char path_resp1[128];
    char path_ccc_resp1[128];
    char path_resp2[128];
    char path_ccc_resp2[128];
    char path_write5[128];
    char path_ratedesc[128];
} gatt_t;

/* ─── Controller State ─── */
typedef struct {
    sd_bus *bus;
    char adapter[128];
    char dev_path[128];
    char bdaddr[18];
    gatt_t gatt;
    int uhid_fd;
    int connected;
    int running;
    uint32_t buttons;       /* 24-bit from report */
    uint16_t lx, ly, rx, ry; /* 12-bit sticks */
    uint8_t dpad;
} ctrl_t;

/* ─── Helpers ─── */
static void build_nwcp(uint8_t *hdr, uint8_t cmd, uint8_t sub, size_t dlen) {
    hdr[0]=cmd; hdr[1]=NWCP_DIR_OUT; hdr[2]=NWCP_TRANS_BT;
    hdr[3]=sub; hdr[4]=0; hdr[5]=dlen&0xFF; hdr[6]=(dlen>>8)&0xFF; hdr[7]=0;
}

static int prop_bool(sd_bus *bus, const char *path, const char *prop) {
    sd_bus_error e=SD_BUS_ERROR_NULL; sd_bus_message *m=NULL;
    int r=sd_bus_get_property(bus,BLUEZ,path,IFACE_DEVICE,prop,&e,&m,"b");
    if(r<0){sd_bus_error_free(&e);return r;}
    int v=0; sd_bus_message_read(m,"b",&v); sd_bus_message_unref(m);
    return v;
}

static int prop_string(sd_bus *bus, const char *path, const char *iface,
                       const char *prop, char *out, size_t sz) {
    sd_bus_error e=SD_BUS_ERROR_NULL; sd_bus_message *m=NULL;
    int r=sd_bus_get_property(bus,BLUEZ,path,iface,prop,&e,&m,"s");
    if(r<0){sd_bus_error_free(&e);return r;}
    const char *s=NULL; sd_bus_message_read(m,"s",&s);
    if(s) snprintf(out,sz,"%s",s); sd_bus_message_unref(m);
    return 0;
}

static int method_call(sd_bus *bus, const char *path, const char *iface,
                       const char *method, const char *sig, ...) {
    sd_bus_error e=SD_BUS_ERROR_NULL; sd_bus_message *m=NULL;
    va_list ap; va_start(ap,sig);
    int r=sd_bus_call_methodv(bus,BLUEZ,path,iface,method,&e,&m,sig,ap);
    va_end(ap);
    if(r<0){ERR("%s.%s: %s",iface,method,e.message); sd_bus_error_free(&e);}
    else if(m) sd_bus_message_unref(m);
    return r;
}

static int gatt_write(sd_bus *bus, const char *path, const uint8_t *data, size_t len) {
    sd_bus_error e=SD_BUS_ERROR_NULL; sd_bus_message *m=NULL;
    int r=sd_bus_call_method(bus,BLUEZ,path,IFACE_GATTCHAR,"WriteValue",&e,&m,"ay",len,data);
    if(r<0){ERR("WriteValue(%s): %s",path,e.message); sd_bus_error_free(&e);}
    else{sd_bus_message_unref(m); DBG("Wrote %zu bytes to %s",len,path);}
    return r;
}

static int ccc_write(sd_bus *bus, const char *desc_path) {
    uint8_t v[]={0x01,0x00};
    sd_bus_error e=SD_BUS_ERROR_NULL; sd_bus_message *m=NULL;
    int r=sd_bus_call_method(bus,BLUEZ,desc_path,IFACE_GATTDESC,"WriteValue",&e,&m,"ay",(size_t)2,v);
    if(r<0){ERR("CCC write(%s): %s",desc_path,e.message); sd_bus_error_free(&e);}
    else{sd_bus_message_unref(m); DBG("CCC enabled on %s",desc_path);}
    return r;
}

static int gatt_notify(sd_bus *bus, const char *path) {
    return method_call(bus,path,IFACE_GATTCHAR,"StartNotify","");
}

static int bt_connect(sd_bus *bus, const char *path) {
    return method_call(bus,path,IFACE_DEVICE,"Connect","");
}

static int bt_disconnect(sd_bus *bus, const char *path) {
    method_call(bus,path,IFACE_DEVICE,"Disconnect",""); return 0;
}

static int adapter_find(sd_bus *bus, char *out, size_t sz) {
    sd_bus_error e=SD_BUS_ERROR_NULL; sd_bus_message *m=NULL;
    int r=sd_bus_call_method(bus,BLUEZ,"/",IFACE_OBJMGR,"GetManagedObjects",&e,&m,"");
    if(r<0){sd_bus_error_free(&e);return r;}
    r=sd_bus_message_enter_container(m,'a',"{oa{sa{sv}}}");
    while(r>0){const char *p=NULL;
        r=sd_bus_message_enter_container(m,'e',"oa{sa{sv}}"); if(r<=0)break;
        sd_bus_message_read(m,"o",&p);
        r=sd_bus_message_enter_container(m,'a',"{sa{sv}}");
        while(r>0){const char *iface=NULL;
            r=sd_bus_message_enter_container(m,'e',"sa{sv}"); if(r<=0)break;
            sd_bus_message_read(m,"s",&iface);
            if(iface&&!strcmp(iface,IFACE_ADAPTER)){snprintf(out,sz,"%s",p);
                sd_bus_message_exit_container(m);sd_bus_message_exit_container(m);
                sd_bus_message_exit_container(m);sd_bus_message_exit_container(m);
                sd_bus_message_unref(m);return 0;}
            sd_bus_message_skip(m,NULL); sd_bus_message_exit_container(m);}
        sd_bus_message_exit_container(m); sd_bus_message_exit_container(m);}
    sd_bus_message_exit_container(m); sd_bus_message_unref(m);
    return -ENODEV;
}

/* Wait for ServicesResolved */
static int wait_services(sd_bus *bus, const char *path, int timeout) {
    for(int i=0;i<timeout*10;i++){int r=prop_bool(bus,path,"ServicesResolved");
        if(r>0)return 0; if(r<0)return r; usleep(100000);}
    return -ETIMEDOUT;
}

/* ─── GATT Discovery ─── */
static int discover_gatt(sd_bus *bus, const char *dev_path, gatt_t *g) {
    sd_bus_error e=SD_BUS_ERROR_NULL; sd_bus_message *m=NULL;
    int r=sd_bus_call_method(bus,BLUEZ,dev_path,IFACE_OBJMGR,"GetManagedObjects",&e,&m,"");
    if(r<0){sd_bus_error_free(&e);return r;}
    memset(g,0,sizeof(*g));
    r=sd_bus_message_enter_container(m,'a',"{oa{sa{sv}}}");
    while(r>0){const char *obj=NULL;
        r=sd_bus_message_enter_container(m,'e',"oa{sa{sv}}"); if(r<=0)break;
        sd_bus_message_read(m,"o",&obj);
        r=sd_bus_message_enter_container(m,'a',"{sa{sv}}");
        while(r>0){const char *iface=NULL;
            r=sd_bus_message_enter_container(m,'e',"sa{sv}"); if(r<=0)break;
            sd_bus_message_read(m,"s",&iface);
            if(iface&&!strcmp(iface,IFACE_GATTCHAR)){
                char uuid[64]={0};
                r=sd_bus_message_enter_container(m,'a',"{sv}");
                while(r>0){const char *prop=NULL;
                    r=sd_bus_message_enter_container(m,'e',"sv"); if(r<=0)break;
                    sd_bus_message_read(m,"s",&prop);
                    if(prop&&!strcmp(prop,"UUID")){
                        const char *u=NULL;
                        sd_bus_message_enter_container(m,'v',"s");
                        sd_bus_message_read(m,"s",&u); if(u)snprintf(uuid,64,"%s",u);
                        sd_bus_message_exit_container(m);}
                    else{sd_bus_message_skip(m,NULL);}
                    sd_bus_message_exit_container(m);}
                sd_bus_message_exit_container(m);
                /* Match UUIDs */
                if(!strcasecmp(uuid,UUID_CH_INPUT)) snprintf(g->path_input,128,"%s",obj);
                else if(!strcasecmp(uuid,UUID_CH_OUTPUT)) snprintf(g->path_output,128,"%s",obj);
                else if(!strcasecmp(uuid,UUID_CH_CMD_OUT)) snprintf(g->path_cmd_out,128,"%s",obj);
                else if(!strcasecmp(uuid,UUID_CH_CMD_DATA)) snprintf(g->path_cmd_data,128,"%s",obj);
                else if(!strcasecmp(uuid,UUID_CH_RESP1)) snprintf(g->path_resp1,128,"%s",obj);
                else if(!strcasecmp(uuid,UUID_CH_RESP2)) snprintf(g->path_resp2,128,"%s",obj);
                else if(!strcasecmp(uuid,UUID_CH_WRITE5)) snprintf(g->path_write5,128,"%s",obj);
                else if(!strcasecmp(uuid,UUID_CH_RATEGO)) snprintf(g->path_ratedesc,128,"%s",obj);
            } else if(iface&&!strcmp(iface,IFACE_GATTDESC)){
                char uuid[64]={0};
                r=sd_bus_message_enter_container(m,'a',"{sv}");
                while(r>0){const char *prop=NULL;
                    r=sd_bus_message_enter_container(m,'e',"sv"); if(r<=0)break;
                    sd_bus_message_read(m,"s",&prop);
                    if(prop&&!strcmp(prop,"UUID")){
                        const char *u=NULL;
                        sd_bus_message_enter_container(m,'v',"s");
                        sd_bus_message_read(m,"s",&u); if(u)snprintf(uuid,64,"%s",u);
                        sd_bus_message_exit_container(m);}
                    else{sd_bus_message_skip(m,NULL);}
                    sd_bus_message_exit_container(m);}
                sd_bus_message_exit_container(m);
                /* CCC descriptors: figure out which char they belong to by path prefix */
                if(!strcasecmp(uuid,UUID_CH_CCC)){
                    char parent[128]; snprintf(parent,128,"%s",obj);
                    char *last=parent; for(char *p=parent;*p;p++)if(*p=='/')last=p+1;
                    /* Match by characteristic path prefix */
                    if(g->path_input[0]&&strstr(obj,g->path_input+strlen(g->path_input)-5))
                        snprintf(g->path_ccc_input,128,"%s",obj);
                    else if(g->path_resp1[0])snprintf(g->path_ccc_resp1,128,"%s",obj);
                    else if(g->path_resp2[0])snprintf(g->path_ccc_resp2,128,"%s",obj);
                }
            } else{sd_bus_message_skip(m,NULL);}
            sd_bus_message_exit_container(m);}
        sd_bus_message_exit_container(m); sd_bus_message_exit_container(m);}
    sd_bus_message_exit_container(m); sd_bus_message_unref(m);
    if(!g->path_input[0]||!g->path_cmd_data[0]){
        ERR("Missing GATT chars: input=%s cmd_data=%s",
            g->path_input[0]?"ok":"MISSING",g->path_cmd_data[0]?"ok":"MISSING");
        return -ENODEV;}
    return 0;
}

/* ─── NWCP Command Sender ─── */
static int nwcp_send_cmd(ctrl_t *c, uint8_t cmd, uint8_t sub,
                         const uint8_t *data, size_t dlen) {
    uint8_t buf[256]; build_nwcp(buf,cmd,sub,dlen);
    if(data&&dlen) memcpy(buf+NWCP_HDR_LEN,data,dlen);
    size_t total=NWCP_HDR_LEN+dlen;
    /* Use cmd_data (0x0016) with 33-byte zero prefix */
    uint8_t framed[256+33];
    memset(framed,0,33);
    memcpy(framed+33,buf,total);
    return gatt_write(c->bus,c->gatt.path_cmd_data,framed,33+total);
}

/* ─── Init Sequence (Pro Controller 2, ndeadly documented) ─── */
static int init_sequence(ctrl_t *c) {
    LOG("Running init sequence...");
    uint8_t tmp[64];

    /* 0x0005 ← 01 00 */
    LOG("  Step 1: Write unknown init");
    uint8_t s1[]={0x01,0x00}; gatt_write(c->bus,c->gatt.path_write5,s1,2);
    usleep(50000);

    /* 0x001F ← 01 00 (CCC for response #2) */
    if(c->gatt.path_ccc_resp2[0]){LOG("  Step 2: CCC resp2");ccc_write(c->bus,c->gatt.path_ccc_resp2);usleep(50000);}
    /* 0x001B ← 01 00 (CCC for response #1) */
    if(c->gatt.path_ccc_resp1[0]){LOG("  Step 2b: CCC resp1");ccc_write(c->bus,c->gatt.path_ccc_resp1);usleep(50000);}

    /* 0x0016 ← 07 91 01 01 00 00 00 00 */
    LOG("  Step 3: Init handshake");
    nwcp_send_cmd(c,CMD_INIT_HANDSHAKE,0x01,NULL,0); usleep(100000);

    /* 0x0016 ← 02 91 01 04 00 08 00 00 40 7e 00 00 00 30 01 00  (read SPI 0x13000) */
    LOG("  Step 4: Read serial from SPI");
    tmp[0]=0x40;tmp[1]=0x7e;tmp[2]=0x00;tmp[3]=0x00;tmp[4]=0x00;tmp[5]=0x30;tmp[6]=0x01;tmp[7]=0x00;
    nwcp_send_cmd(c,CMD_FLASH_READ,0x04,tmp,8); usleep(100000);

    /* 0x0016 ← 16 91 01 01 00 00 00 00 */
    LOG("  Step 5: Unknown 0x16");
    nwcp_send_cmd(c,CMD_UNKNOWN_16,0x01,NULL,0); usleep(100000);

    /* 0x0016 ← 0a 91 01 02 00 04 00 00 03 00 00 00  (vibration sample) */
    LOG("  Step 6: Vibration sample");
    tmp[0]=0x03;tmp[1]=0x00;tmp[2]=0x00;tmp[3]=0x00;
    nwcp_send_cmd(c,CMD_VIBRATE,0x02,tmp,4); usleep(100000);

    /* 0x0016 ← 09 91 01 07 00 08 00 00 01 00 00 00 00 00 00 00  (player LED) */
    LOG("  Step 7: Player LED");
    tmp[0]=0x01;memset(tmp+1,0,7);
    nwcp_send_cmd(c,CMD_LED,SUB_LED_PATTERN,tmp,8); usleep(100000);

    /* 0x0016 ← 0c 91 01 02 00 04 00 00 2f 00 00 00  (feature mask 0x2F) */
    LOG("  Step 8: Feature mask 0x2F");
    tmp[0]=0x2f;tmp[1]=0x00;tmp[2]=0x00;tmp[3]=0x00;
    nwcp_send_cmd(c,CMD_FEATSEL,SUB_FEAT_SET_MASK,tmp,4); usleep(100000);

    /* Flash reads for calibration */
    LOG("  Step 9: Read calib data");
    tmp[0]=0x40;tmp[1]=0x7e;tmp[2]=0x00;tmp[3]=0x00;tmp[4]=0x80;tmp[5]=0x30;tmp[6]=0x01;tmp[7]=0x00;
    nwcp_send_cmd(c,CMD_FLASH_READ,0x04,tmp,8); usleep(100000);
    tmp[0]=0x40;tmp[1]=0x7e;tmp[2]=0x00;tmp[3]=0x00;tmp[4]=0xc0;tmp[5]=0x30;tmp[6]=0x01;tmp[7]=0x00;
    nwcp_send_cmd(c,CMD_FLASH_READ,0x04,tmp,8); usleep(100000);
    tmp[0]=0x40;tmp[1]=0x7e;tmp[2]=0x00;tmp[3]=0x00;tmp[4]=0x40;tmp[5]=0xc0;tmp[6]=0x1f;tmp[7]=0x00;
    nwcp_send_cmd(c,CMD_FLASH_READ,0x04,tmp,8); usleep(100000);
    tmp[0]=0x10;tmp[1]=0x7e;tmp[2]=0x00;tmp[3]=0x00;tmp[4]=0x40;tmp[5]=0x30;tmp[6]=0x01;tmp[7]=0x00;
    nwcp_send_cmd(c,CMD_FLASH_READ,0x04,tmp,8); usleep(100000);
    tmp[0]=0x18;tmp[1]=0x7e;tmp[2]=0x00;tmp[3]=0x00;tmp[4]=0x00;tmp[5]=0x31;tmp[6]=0x01;tmp[7]=0x00;
    nwcp_send_cmd(c,CMD_FLASH_READ,0x04,tmp,8); usleep(100000);

    /* 0x0016 ← 11 91 01 03 00 00 00 00 */
    LOG("  Step 10: Unknown 0x11");
    nwcp_send_cmd(c,CMD_UNKNOWN_11,0x03,NULL,0); usleep(100000);

    /* Flash read 0x13060 */
    tmp[0]=0x20;tmp[1]=0x7e;tmp[2]=0x00;tmp[3]=0x00;tmp[4]=0x60;tmp[5]=0x30;tmp[6]=0x01;tmp[7]=0x00;
    nwcp_send_cmd(c,CMD_FLASH_READ,0x04,tmp,8); usleep(100000);

    /* Vibration + feature confirm */
    LOG("  Step 11: Vibration + feature confirm");
    uint8_t vib[]={0x01,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x35,0x00,0x46,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    nwcp_send_cmd(c,CMD_VIBRATE,0x08,vib,20); usleep(100000);

    tmp[0]=0x2f;tmp[1]=0x00;tmp[2]=0x00;tmp[3]=0x00;
    nwcp_send_cmd(c,CMD_FEATSEL,SUB_FEAT_ENABLE,tmp,4); usleep(100000);

    /* 0x0010 ← 85 00 (report rate descriptor) */
    if(c->gatt.path_ratedesc[0]){
        LOG("  Step 12: Report rate");
        gatt_write(c->bus,c->gatt.path_ratedesc,(uint8_t[]){0x85,0x00},2); usleep(50000);
    }

    /* 0x000F ← 01 00 (CCC for input reports) */
    LOG("  Step 13: Enable input notifications");
    if(c->gatt.path_ccc_input[0])ccc_write(c->bus,c->gatt.path_ccc_input);

    LOG("Init sequence complete");
    return 0;
}

/* ─── Input Report Parser (Pro Controller 0x09, 63 bytes) ─── */
static void parse_input(ctrl_t *c, const uint8_t *d, size_t len) {
    if(len<11)return;
    /* Byte 0: counter, Byte 1: power info */
    /* Bytes 2-4: 24-bit buttons */
    c->buttons=d[2]|(d[3]<<8)|(d[4]<<16);
    /* Bytes 5-7: left stick (12-bit packed) */
    c->lx=((d[5]<<4)|(d[6]>>4))&0xFFF;
    c->ly=((d[7]<<4)|(d[6]&0x0F))&0xFFF;
    /* Bytes 8-10: right stick (12-bit packed) */
    c->rx=((d[8]<<4)|(d[9]>>4))&0xFFF;
    c->ry=((d[10]<<4)|(d[9]&0x0F))&0xFFF;
    /* D-pad from button bits in byte 1: 0x01=Down,0x02=Right,0x04=Left,0x08=Up
     * HID hat switch: 0=N,1=NE,2=E,3=SE,4=S,5=SW,6=W,7=NW,8=center */
    uint8_t dp=(d[3]&0x0F);
    switch(dp){
        case 0x00: c->dpad=8; break; /* center */
        case 0x01: c->dpad=4; break; /* Down */
        case 0x02: c->dpad=2; break; /* Right */
        case 0x04: c->dpad=6; break; /* Left */
        case 0x08: c->dpad=0; break; /* Up */
        case 0x0A: c->dpad=1; break; /* Up+Right */
        case 0x0C: c->dpad=7; break; /* Up+Left */
        case 0x03: c->dpad=3; break; /* Down+Right */
        case 0x05: c->dpad=5; break; /* Down+Left */
        default:   c->dpad=8; break;
    }
}

/* Button mapping for HID report (ndeadly Input Report 0x09):
 * Byte 0 (d[2]): 0x01=B,0x02=A,0x04=Y,0x08=X,0x10=R,0x20=ZR,0x40=Plus,0x80=RStick
 * Byte 1 (d[3]): 0x01=Down,0x02=Right,0x04=Left,0x08=Up,0x10=L,0x20=ZL,0x40=Minus,0x80=LStick
 * Byte 2 (d[4]): 0x01=Home,0x02=Capture,0x04=GR,0x08=GL,0x10=C */
static void build_hid_report(ctrl_t *c, uint8_t *r) {
    memset(r,0,HID_INPUT_REPORT_SIZE);
    r[0]=0x01; /* Report ID */
    uint32_t b=0;
    /* Nintendo-native bit order (matches ndeadly/switch2_input_viewer):
     * Byte0: 0x01=B,0x02=A,0x04=Y,0x08=X,0x10=R,0x20=ZR,0x40=+,0x80=RStick
     * Byte1: 0x01=Down,0x02=Right,0x04=Left,0x08=Up,0x10=L,0x20=ZL,0x40=-,0x80=LStick
     * Byte2: 0x01=Home,0x02=Capture,0x04=GR,0x08=GL,0x10=C */
    if(c->buttons&0x00000001)b|=0x0001; /* B→btn1 */
    if(c->buttons&0x00000002)b|=0x0002; /* A→btn2 */
    if(c->buttons&0x00000004)b|=0x0004; /* Y→btn3 */
    if(c->buttons&0x00000008)b|=0x0008; /* X→btn4 */
    if(c->buttons&0x00000010)b|=0x0010; /* R→btn5 */
    if(c->buttons&0x00000020)b|=0x0020; /* ZR→btn6 */
    if(c->buttons&0x00000040)b|=0x0040; /* +→btn7 */
    if(c->buttons&0x00000080)b|=0x0080; /* RStick→btn8 */
    if(c->buttons&0x00000100)b|=0x0100; /* Down→btn9 */
    if(c->buttons&0x00000200)b|=0x0200; /* Right→btn10 */
    if(c->buttons&0x00000400)b|=0x0400; /* Left→btn11 */
    if(c->buttons&0x00000800)b|=0x0800; /* Up→btn12 */
    if(c->buttons&0x00001000)b|=0x1000; /* L→btn13 */
    if(c->buttons&0x00002000)b|=0x2000; /* ZL→btn14 */
    if(c->buttons&0x00004000)b|=0x4000; /* -→btn15 */
    if(c->buttons&0x00008000)b|=0x8000; /* LStick→btn16 */
    /* Byte 2: Home(0x10000)→btn17, Capture(0x20000)→btn18,
     * GR(0x40000)→btn19, GL(0x80000)→btn20, C(0x100000)→btn21 */
    if(c->buttons&0x00010000)b|=0x00010000; /* Home→btn17 */
    if(c->buttons&0x00020000)b|=0x00020000; /* Capture→btn18 */
    if(c->buttons&0x00040000)b|=0x00040000; /* GR→btn19 */
    if(c->buttons&0x00080000)b|=0x00080000; /* GL→btn20 */
    if(c->buttons&0x00100000)b|=0x00100000; /* C→btn21 */
    r[1]=b&0xFF; r[2]=(b>>8)&0xFF; r[3]=(b>>16)&0xFF;
    r[4]=c->lx&0xFF; r[5]=(c->lx>>8)&0xFF;
    r[6]=c->ly&0xFF; r[7]=(c->ly>>8)&0xFF;
    r[8]=c->rx&0xFF; r[9]=(c->rx>>8)&0xFF;
    r[10]=c->ry&0xFF; r[11]=(c->ry>>8)&0xFF;
    r[12]=c->dpad&0x0F;
}

/* ─── UHID ─── */
static int uhid_create(int fd) {
    struct uhid_event ev={.type=UHID_CREATE};
    snprintf((char*)ev.u.create.name,128,"Nintendo Switch 2 Pro Controller");
    ev.u.create.bus=BUS_BLUETOOTH; ev.u.create.vendor=VID_NINTENDO;
    ev.u.create.product=0x2060; ev.u.create.version=0; ev.u.create.country=0;
    ev.u.create.rd_size=HID_REPORT_DESC_SIZE;
    memcpy(ev.u.create.rd_data,hid_report_descriptor,
           HID_REPORT_DESC_SIZE<sizeof(ev.u.create.rd_data)?HID_REPORT_DESC_SIZE:sizeof(ev.u.create.rd_data));
    if(write(fd,&ev,sizeof(ev))!=(ssize_t)sizeof(ev)){ERR("UHID_CREATE: %s",strerror(errno));return-1;}
    LOG("UHID device created"); return 0;
}

static void uhid_destroy(int fd) {
    struct uhid_event ev={.type=UHID_DESTROY}; write(fd,&ev,sizeof(ev));
}

static void uhid_input(int fd, const uint8_t *r, size_t len) {
    struct uhid_event ev={.type=UHID_INPUT};
    ev.u.input.size=len; memcpy(ev.u.input.data,r,len<sizeof(ev.u.input.data)?len:sizeof(ev.u.input.data));
    write(fd,&ev,sizeof(ev));
}

/* ─── PropertiesChanged Handler ─── */
static int dbus_filter(sd_bus_message *m, void *ud, sd_bus_error *e) {
    (void)e; ctrl_t *c=ud;
    if(sd_bus_message_is_signal(m,IFACE_PROPS,"PropertiesChanged")){
        const char *iface=NULL; sd_bus_message_read(m,"s",&iface);
        if(iface&&!strcmp(iface,IFACE_GATTCHAR)){
            const char *path=sd_bus_message_get_path(m);
            if(path&&!strcmp(path,c->gatt.path_input)){
                /* Value changed → parse input report */
                int r=sd_bus_message_enter_container(m,'a',"{sv}");
                while(r>0){const char *prop=NULL;
                    r=sd_bus_message_enter_container(m,'e',"sv"); if(r<=0)break;
                    sd_bus_message_read(m,"s",&prop);
                    if(prop&&!strcmp(prop,"Value")){
                        const uint8_t *val=NULL; size_t vlen=0;
                        sd_bus_message_enter_container(m,'v',"ay");
                        sd_bus_message_read_array(m,'y',(const void**)&val,&vlen);
                        if(val&&vlen){parse_input(c,val,vlen);
                            uint8_t rpt[HID_INPUT_REPORT_SIZE];
                            build_hid_report(c,rpt);
                            if(c->uhid_fd>=0)uhid_input(c->uhid_fd,rpt,HID_INPUT_REPORT_SIZE);}
                        sd_bus_message_exit_container(m);}
                    else sd_bus_message_skip(m,NULL);
                    sd_bus_message_exit_container(m);}
                sd_bus_message_exit_container(m);
            }
        } else if(iface&&!strcmp(iface,IFACE_DEVICE)){
            int r=sd_bus_message_enter_container(m,'a',"{sv}");
            while(r>0){const char *prop=NULL;
                r=sd_bus_message_enter_container(m,'e',"sv"); if(r<=0)break;
                sd_bus_message_read(m,"s",&prop);
                if(prop&&!strcmp(prop,"Connected")){
                    int conn=1; sd_bus_message_enter_container(m,'v',"b");
                    sd_bus_message_read(m,"b",&conn); sd_bus_message_exit_container(m);
                    if(!conn){LOG("Disconnected");c->connected=0;}}
                else sd_bus_message_skip(m,NULL);
                sd_bus_message_exit_container(m);}
            sd_bus_message_exit_container(m);
        }
    }
    return 0;
}

/* ─── Main Loop ─── */
static int event_loop(ctrl_t *c) {
    int sfd;{sigset_t m;sigemptyset(&m);sigaddset(&m,SIGINT);sigaddset(&m,SIGTERM);
        sigprocmask(SIG_BLOCK,&m,NULL); sfd=signalfd(-1,&m,SFD_NONBLOCK|SFD_CLOEXEC);}
    struct pollfd fds[3]={{.fd=c->uhid_fd,.events=POLLIN},
                          {.fd=sd_bus_get_fd(c->bus),.events=POLLIN},
                          {.fd=sfd,.events=POLLIN}};
    c->running=1;
    while(c->running&&c->connected){
        while(sd_bus_process(c->bus,NULL)>0);
        if(poll(fds,3,500)<0&&errno!=EINTR)break;
        if(!c->connected)break;
        if(fds[0].revents&POLLIN){struct uhid_event ev; ssize_t n=read(c->uhid_fd,&ev,sizeof(ev));
            if(n>=(ssize_t)sizeof(uint32_t)&&ev.type==UHID_OUTPUT&&ev.u.output.size>=3){
                uint8_t l=ev.u.output.data[1],r=ev.u.output.data[2];
                if(c->gatt.path_output[0]){
                    uint8_t rumble[]={0x00,l,r}; /* Report ID 0x00 for BT (no report ID) */
                    gatt_write(c->bus,c->gatt.path_output,rumble,3);}}}
        if(fds[1].revents&POLLIN)while(sd_bus_process(c->bus,NULL)>0);
        if(fds[2].revents&POLLIN){struct signalfd_siginfo si; read(sfd,&si,sizeof(si));
            LOG("Signal %d, shutting down",si.ssi_signo);c->running=0;}
    }
    close(sfd); return 0;
}

/* ─── Connect + Init ─── */
static int connect_init(ctrl_t *c) {
    LOG("Connecting to %s...",c->bdaddr);
    if(bt_connect(c->bus,c->dev_path)<0)return-1;
    LOG("Waiting for GATT services...");
    if(wait_services(c->bus,c->dev_path,15)<0){ERR("ServicesResolved timeout");bt_disconnect(c->bus,c->dev_path);return-1;}
    LOG("Discovering GATT...");
    if(discover_gatt(c->bus,c->dev_path,&c->gatt)<0){bt_disconnect(c->bus,c->dev_path);return-1;}
    c->uhid_fd=open("/dev/uhid",O_RDWR);
    if(c->uhid_fd<0){ERR("/dev/uhid: %s",strerror(errno));bt_disconnect(c->bus,c->dev_path);return-1;}
    if(uhid_create(c->uhid_fd)<0){close(c->uhid_fd);bt_disconnect(c->bus,c->dev_path);return-1;}
    if(init_sequence(c)<0){uhid_destroy(c->uhid_fd);close(c->uhid_fd);bt_disconnect(c->bus,c->dev_path);return-1;}
    if(gatt_notify(c->bus,c->gatt.path_input)<0){LOG("StartNotify failed (may already be active)");}
    c->connected=1; LOG("Controller ready: %s",c->bdaddr); return 0;
}

static void cleanup(ctrl_t *c) {
    if(c->uhid_fd>=0){uhid_destroy(c->uhid_fd);close(c->uhid_fd);c->uhid_fd=-1;}
    if(c->dev_path[0])bt_disconnect(c->bus,c->dev_path);
    c->connected=0;
}

/* ─── Main ─── */
static void usage(const char *p){fprintf(stderr,
    "Usage: %s -a BD_ADDR [options]\n"
    "  -a ADDR  Controller Bluetooth address (required)\n"
    "  -v       Verbose\n"
    "  -h       Help\n",p);}

int main(int argc, char **argv) {
    char *addr=NULL; int opt;
    while((opt=getopt(argc,argv,"a:vh"))!=-1){switch(opt){
        case'a':addr=optarg;break; case'v':g_verbose=1;break;
        case'h':usage(argv[0]);return 0; default:usage(argv[0]);return 1;}}
    if(!addr){ERR("--addr required");usage(argv[0]);return 1;}

    ctrl_t c; memset(&c,0,sizeof(c)); c.uhid_fd=-1;
    c.lx=c.ly=c.rx=c.ry=2048; c.dpad=8;

    if(sd_bus_open_system(&c.bus)<0){ERR("D-Bus connect failed");return 1;}
    sd_bus_add_filter(c.bus,NULL,dbus_filter,&c);

    if(adapter_find(c.bus,c.adapter,sizeof(c.adapter))<0){ERR("No adapter");sd_bus_unref(c.bus);return 1;}
    LOG("Adapter: %s",c.adapter);

    /* Build device path from BD_ADDR */
    snprintf(c.bdaddr,18,"%s",addr);
    char canon[18]; snprintf(canon,18,"%s",addr);
    for(char *p=canon;*p;p++)if(*p==':')*p='_';
    snprintf(c.dev_path,128,"%s/dev_%s",c.adapter,canon);

    /* Reconnect loop */
    int delay=1;
    for(int attempt=1;;attempt++){
        if(attempt>1){LOG("Reconnecting in %ds...",delay);sleep(delay);
            if(delay<30)delay*=2;}
        if(connect_init(&c)<0){LOG("Connection failed, retrying...");continue;}
        delay=1; event_loop(&c); cleanup(&c);
        if(!c.running)break;
        close(c.uhid_fd); c.uhid_fd=-1; /* reset for next connect_init */
    }
    if(c.uhid_fd>=0)close(c.uhid_fd);
    sd_bus_unref(c.bus); return 0;
}
