# Proposal: NET and BLE interfaces for PAPPs

Status: **NET v1 is implemented** (`components/papp_abi/include/papp_abi.h`, `components/lx_loader/lx_svc_net.c`,
test app `apps/net_test`); the header is the reference, the sketch below differs in details
(`socket`/`accept` return an error through a pointer, `local_addr`, `PAPP_SO_REUSEADDR`, TLS `insecure`).
Joining another network is for the run only. The SoftAP runs beside the station: a PC joined it, pinged the board
and fetched pages from an app's TCP server at 192.168.4.1.

**BLE v1 is implemented** (`components/lx_loader/lx_svc_ble.c`, test app `apps/ble_test`) on the Guition boards:
NimBLE over esp_hosted 2.12.13 needs `esp_hosted_bt_controller_init` + `_enable` before `nimble_port_init`
(without them HCI times out, which is what the reference project ran into). Differences from the sketch below:
`own_addr` returns an error code, `mtu_exchange` takes a timeout and returns the MTU, `set_name`, an
`PAPP_BLE_EV_ADV_DONE` event, `notify` also stores the value and sends an indication to peers that asked for one,
and `gatt_register` restarts the host so it is best called first. Tested: a Windows PC (bleak, the Gution
`ble_client.py`) against `ble_test periph`, and board to board (`central` on the 7" against `periph` on the 4.3").

## Ground rules

- **Two interfaces, both optional imports.** `NET ` (Wi-Fi, sockets, TLS, HTTP) and `BLE ` (Bluetooth LE: GAP and GATT).
  - A board without the radio, or without BLE in its C6 firmware, leaves the slot NULL.
  - `ARCHITECTURE.md` reserved `BTHD` for BLE HID gamepads. That stays a launcher feature feeding INPT, not part of the app's BLE.
- **The radio follows the imports.** Today the launcher pauses Wi-Fi while an app runs, which saves RAM and SDIO time.
  - An app that imports NET keeps Wi-Fi running: the loader sees the import and skips the pause.
  - NimBLE starts only for an app that imports BLE, and stops at teardown.
  - Apps that import neither keep what they have now.
- **No callbacks into app code.**
  - Everything asynchronous arrives as events in a queue the app polls, like INPT's `poll_event`. Launcher tasks with internal stacks never run app code, and teardown never waits on an app callback.
  - Blocking calls take a timeout, wait in slices and return `PAPP_E_EXIT` when the app is asked to stop, so a forced stop is never stuck in `recv`.
- **Everything is tracked and reclaimed at teardown, using the usual SVC_ENTER/SVC_LEAVE tracking:**
  - sockets, TLS sessions and HTTP requests are closed
  - a scan is stopped
  - a SoftAP is stopped
  - a temporary network is left and the saved network rejoined
  - for BLE: advertising and scans stop, connections close, the GATT table is removed and NimBLE is shut down
- **Own types and constants, never lwIP or NimBLE ones.** Errors are negative `PAPP_E_*` values. The SDK adds a BSD socket shim (`<sys/socket.h>`, `getaddrinfo`, int file descriptors) on top of NET, so ported code builds unchanged.

## NET v1

```c
#define PAPP_IF_NET       PAPP_FOURCC('N', 'E', 'T', ' ')
#define PAPP_IF_NET_VER   1

enum {                                   /* errors: negative return values */
    PAPP_E_OK = 0, PAPP_E_TIMEOUT = -1, PAPP_E_EXIT = -2, PAPP_E_NOMEM = -3, PAPP_E_LIMIT = -4,
    PAPP_E_NOT_CONNECTED = -5, PAPP_E_REFUSED = -6, PAPP_E_RESET = -7, PAPP_E_DNS = -8,
    PAPP_E_TLS = -9, PAPP_E_CERT = -10, PAPP_E_ARG = -11, PAPP_E_UNSUPPORTED = -12, PAPP_E_IO = -13,
};

/* Wi-Fi */
enum { PAPP_NET_OFF, PAPP_NET_IDLE, PAPP_NET_CONNECTING, PAPP_NET_CONNECTED, PAPP_NET_FAILED };

typedef struct {
    uint8_t  state;                      /* PAPP_NET_*                               */
    int8_t   rssi;                       /* dBm, 0 if not connected                  */
    uint8_t  channel;
    uint8_t  ap_running;                 /* SoftAP up                                */
    char     ssid[33];
    uint8_t  ip[4], mask[4], gateway[4], dns[4];
    uint8_t  mac[6];
} papp_net_info_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
    uint8_t secure;                      /* needs a password                         */
} papp_net_ap_t;

/* Addresses: room for IPv6 later */
enum { PAPP_AF_INET = 4, PAPP_AF_INET6 = 6 };
typedef struct {
    uint8_t  family;
    uint8_t  reserved;
    uint16_t port;
    uint8_t  ip[16];                     /* IPv4 in ip[0..3]                         */
} papp_addr_t;

typedef struct papp_sock papp_sock_t;
enum { PAPP_SOCK_TCP = 1, PAPP_SOCK_UDP = 2 };

enum {                                   /* set_opt */
    PAPP_SO_NODELAY = 1,                 /* TCP, value 0/1                           */
    PAPP_SO_BROADCAST,                   /* UDP, value 0/1                           */
    PAPP_SO_KEEPALIVE,                   /* TCP, seconds, 0 = off                    */
    PAPP_SO_MCAST_JOIN,                  /* UDP, value = IPv4 group address          */
    PAPP_SO_MCAST_LEAVE,
};

enum { PAPP_POLL_IN = 1, PAPP_POLL_OUT = 2, PAPP_POLL_ERR = 4 };
typedef struct { papp_sock_t *sock; uint8_t want, got; } papp_poll_t;

/* TLS: tls_connect returns a socket that send/recv/close work on */
typedef struct {
    const char    *ca_pem;               /* NULL = the launcher's certificate bundle */
    size_t         ca_len;
    const char   **alpn;                 /* NULL-terminated, e.g. {"h2", NULL}; may be NULL */
    uint32_t       timeout_ms;
} papp_tls_cfg_t;

/* HTTP(S) client: open, read the body, close */
typedef struct papp_http papp_http_t;
typedef struct {
    const char *method;                  /* "GET", "POST", ...; NULL = GET           */
    const char *url;                     /* http:// or https://                      */
    const char *headers;                 /* "Name: value\r\n..." or NULL             */
    const void *body;                    /* sent as is; NULL = none                  */
    size_t      body_len;
    uint32_t    timeout_ms;              /* per wait; 0 = 15 s                       */
    uint8_t     max_redirects;           /* 0 = 5                                    */
} papp_http_req_t;

/* Events (poll_event) */
enum {
    PAPP_NET_EV_CONNECTED = 1,           /* got an address                          */
    PAPP_NET_EV_DISCONNECTED,            /* code = Wi-Fi reason                      */
    PAPP_NET_EV_SCAN_DONE,               /* code = networks found                    */
    PAPP_NET_EV_AP_CLIENT_JOINED,        /* SoftAP: code = clients now               */
    PAPP_NET_EV_AP_CLIENT_LEFT,
};
typedef struct { uint8_t type; uint8_t reserved; uint16_t code; uint32_t time_ms; } papp_net_event_t;

typedef struct {
    papp_iface_hdr_t hdr;

    /* Wi-Fi station. up() joins the launcher's saved network (Settings > Wi-Fi) and waits for an address. */
    int   (*up)(uint32_t timeout_ms);                         /* PAPP_E_OK when connected      */
    void  (*info)(papp_net_info_t *out);
    int   (*scan)(papp_net_ap_t *out, int max, uint32_t timeout_ms);   /* count, strongest first */
    /* Join another network for this run only: the saved one comes back at teardown, nothing is stored */
    int   (*join)(const char *ssid, const char *password, uint32_t timeout_ms);
    bool  (*poll_event)(papp_net_event_t *out);

    /* SoftAP (local multiplayer, setup pages). Returns PAPP_E_UNSUPPORTED where the C6 firmware can't. */
    int   (*ap_start)(const char *ssid, const char *password, uint8_t channel, uint8_t max_clients);
    void  (*ap_stop)(void);

    /* Names */
    int   (*resolve)(const char *host, papp_addr_t *out, uint32_t timeout_ms);

    /* Sockets */
    papp_sock_t *(*socket)(int type);                         /* NULL: PAPP_E_LIMIT / NOMEM    */
    int   (*connect)(papp_sock_t *s, const papp_addr_t *to, uint32_t timeout_ms);
    int   (*bind)(papp_sock_t *s, uint16_t port);             /* any local address             */
    int   (*listen)(papp_sock_t *s, int backlog);
    papp_sock_t *(*accept)(papp_sock_t *s, papp_addr_t *from, uint32_t timeout_ms);
    int   (*send)(papp_sock_t *s, const void *buf, size_t len, uint32_t timeout_ms);   /* bytes sent */
    int   (*recv)(papp_sock_t *s, void *buf, size_t len, uint32_t timeout_ms);         /* 0 = closed */
    int   (*sendto)(papp_sock_t *s, const void *buf, size_t len, const papp_addr_t *to);
    int   (*recvfrom)(papp_sock_t *s, void *buf, size_t len, papp_addr_t *from, uint32_t timeout_ms);
    int   (*set_opt)(papp_sock_t *s, int opt, int value);
    int   (*poll)(papp_poll_t *set, int n, uint32_t timeout_ms);   /* sockets ready             */
    void  (*close)(papp_sock_t *s);

    /* TLS over TCP (mbedTLS in the launcher: an app doesn't carry 200 KB of it) */
    papp_sock_t *(*tls_connect)(const char *host, uint16_t port, const papp_tls_cfg_t *cfg, int *err);

    /* HTTP(S) */
    papp_http_t *(*http_open)(const papp_http_req_t *req, int *status);   /* after redirects   */
    int   (*http_header)(papp_http_t *h, const char *name, char *out, size_t out_len);
    long  (*http_length)(papp_http_t *h);                      /* -1 = chunked / unknown        */
    int   (*http_read)(papp_http_t *h, void *buf, size_t len); /* 0 = end                       */
    void  (*http_close)(papp_http_t *h);
} papp_if_net_t;
```

Limits per app: 8 sockets, 2 TLS sessions, 2 HTTP requests (each TLS session needs about 40 KB; it goes in PSRAM where mbedTLS allows it).

Left for later versions, appended: WebSocket client, mDNS (`<name>.local` and service discovery), ESP-NOW (needs esp_hosted support), IPv6, server-side TLS.

## BLE v1

The launcher side needs some work first:
- Bluetooth turned on in esp_hosted (HCI over SDIO to the C6) and NimBLE on the P4.
- C6 firmware with BT. We build our own, so the Guition boards are covered. The Tab5 and Waveshare keep their makers' firmware, so the import stays optional there.
  (Done: M5Stack's Tab5 C6 firmware reports "HCI over SDIO, BLE only" at boot, so BLE is on for the Tab5 too, on
  esp_hosted 1.4, where the C6 starts its controller by itself. Tested: scan, the Tab5 as peripheral for the 7" and
  for the PC, 5 start/stop cycles, NET afterwards. The Waveshare 3.5" is untested and stays without BLE.
  The Waveshare dev kit (`waveshare_p4_rpi_dsi`) has BLE once its C6 runs the release's 2.12.13 firmware
  (Settings > Update): scan, start/stop cycles, NET afterwards. With the module's own C6 firmware
  `available()` is false after about 5 s and Wi-Fi is unaffected.)

NimBLE costs about 60–100 KB of RAM. It starts when a BLE app loads and is freed at teardown.

```c
#define PAPP_IF_BLE       PAPP_FOURCC('B', 'L', 'E', ' ')
#define PAPP_IF_BLE_VER   1

typedef struct { uint8_t type; uint8_t a[6]; } papp_ble_addr_t;       /* type: 0 public, 1 random */
typedef struct { uint8_t len; uint8_t u[16]; } papp_ble_uuid_t;       /* len 2, 4 or 16 bytes     */
typedef uint16_t papp_ble_conn_t;                                     /* 0xFFFF = none            */

/* Scanning (central) */
typedef struct {
    uint8_t  active;                     /* ask for scan responses                    */
    uint8_t  filter_duplicates;
    uint16_t interval_ms, window_ms;     /* 0 = defaults                              */
    uint32_t duration_ms;                /* 0 = until scan_stop                       */
} papp_ble_scan_cfg_t;

/* Advertising (peripheral) */
typedef struct {
    const uint8_t *adv;  uint8_t adv_len;      /* raw AD structures, <= 31 bytes       */
    const uint8_t *rsp;  uint8_t rsp_len;      /* scan response, <= 31 bytes           */
    uint8_t  connectable;
    uint16_t interval_ms;                      /* 0 = 100 ms                           */
} papp_ble_adv_cfg_t;

/* GATT server: the app describes its services once; the launcher answers reads from the stored value */
enum {
    PAPP_BLE_READ = 1, PAPP_BLE_WRITE = 2, PAPP_BLE_WRITE_NR = 4,   /* write without response   */
    PAPP_BLE_NOTIFY = 8, PAPP_BLE_INDICATE = 16,
};
typedef struct {
    papp_ble_uuid_t uuid;
    uint16_t        flags;               /* PAPP_BLE_*                                 */
    uint16_t        max_len;             /* value size, <= 512                          */
    uint16_t       *handle;              /* filled in by gatt_register                  */
} papp_ble_chr_t;
typedef struct {
    papp_ble_uuid_t       uuid;
    const papp_ble_chr_t *chrs;
    uint8_t               chr_count;
} papp_ble_svc_t;

/* GATT client discovery */
typedef struct {
    papp_ble_uuid_t uuid;
    uint16_t        handle;              /* value handle                                */
    uint16_t        cccd;                /* 0 if it has none (no notify/indicate)       */
    uint16_t        flags;               /* PAPP_BLE_*                                  */
    uint16_t        svc_index;           /* which entry of the services array           */
} papp_ble_found_chr_t;
typedef struct { papp_ble_uuid_t uuid; uint16_t start, end; } papp_ble_found_svc_t;

/* Events (poll_event). Data is copied into the event, up to 244 bytes (more: read the value). */
enum {
    PAPP_BLE_EV_ADV = 1,                 /* addr, rssi, data = AD structures            */
    PAPP_BLE_EV_SCAN_DONE,
    PAPP_BLE_EV_CONNECTED,               /* conn, addr; code = 0 or an error            */
    PAPP_BLE_EV_DISCONNECTED,            /* conn, code = reason                         */
    PAPP_BLE_EV_NOTIFY,                  /* client: conn, handle, data                  */
    PAPP_BLE_EV_WRITE,                   /* server: a peer wrote conn, handle, data     */
    PAPP_BLE_EV_SUBSCRIBE,               /* server: code = 0 off, 1 notify, 2 indicate  */
    PAPP_BLE_EV_MTU,                     /* code = new MTU                              */
};
typedef struct {
    uint8_t         type;
    int8_t          rssi;
    papp_ble_conn_t conn;
    uint16_t        handle;
    uint16_t        code;
    papp_ble_addr_t addr;
    uint16_t        len;
    uint8_t         data[244];
} papp_ble_event_t;

typedef struct {
    papp_iface_hdr_t hdr;
    bool  (*available)(void);            /* false: no BLE on this board's C6 firmware        */
    void  (*own_addr)(papp_ble_addr_t *out);
    bool  (*poll_event)(papp_ble_event_t *out);
    bool  (*wait_event)(papp_ble_event_t *out, uint32_t timeout_ms);

    /* Central */
    int   (*scan_start)(const papp_ble_scan_cfg_t *cfg);
    void  (*scan_stop)(void);
    int   (*connect)(const papp_ble_addr_t *peer, uint32_t timeout_ms, papp_ble_conn_t *out);
    void  (*disconnect)(papp_ble_conn_t c);
    int   (*mtu_exchange)(papp_ble_conn_t c, uint16_t mtu);
    int   (*discover)(papp_ble_conn_t c, papp_ble_found_svc_t *svcs, int max_svcs, int *n_svcs,
                      papp_ble_found_chr_t *chrs, int max_chrs, int *n_chrs, uint32_t timeout_ms);
    int   (*read)(papp_ble_conn_t c, uint16_t handle, void *buf, size_t len, uint32_t timeout_ms);
    int   (*write)(papp_ble_conn_t c, uint16_t handle, const void *buf, size_t len, bool response,
                   uint32_t timeout_ms);
    int   (*subscribe)(papp_ble_conn_t c, uint16_t cccd, int mode, uint32_t timeout_ms);  /* 0/1/2 */

    /* Peripheral */
    int   (*gatt_register)(const papp_ble_svc_t *svcs, int count);   /* once per run         */
    int   (*set_value)(uint16_t handle, const void *buf, size_t len);   /* what reads return */
    int   (*notify)(papp_ble_conn_t c, uint16_t handle, const void *buf, size_t len);  /* 0xFFFF = all subscribed */
    int   (*adv_start)(const papp_ble_adv_cfg_t *cfg);
    void  (*adv_stop)(void);
} papp_if_ble_t;
```

The SDK adds helpers that aren't part of the interface: building and parsing AD structures (name, flags, UUIDs, manufacturer data) and 16-bit UUID macros.

Limits per app: 3 connections, one GATT table (up to 8 services, 32 characteristics), and an event queue of 32 (the oldest ADV events are dropped first).

Left for BLE v2: pairing and bonding (encrypted characteristics, keys saved on the card), L2CAP channels, extended advertising, and a connection parameter update.

## Launcher work this needs

| | NET | BLE |
|---|---|---|
| Radio | skip `lx_net_pause` for NET apps; `join` rejoins the saved network at teardown | esp_hosted BT config, NimBLE host, C6 firmware with BT (`build_c6_fw.py` config) |
| Service | `lx_svc_net.c`: lwIP sockets behind handles, esp-tls, esp_http_client | `lx_svc_ble.c`: NimBLE GAP/GATT, event queue, start and stop per run |
| Blocking calls | select in 100 ms slices that check the exit flag | the same, on NimBLE's completion semaphores |
| SDK | `papp/net.h`, BSD socket shim in the libc glue | `papp/ble.h`, AD/UUID helpers |
| Tests | `apps/net_test` (DNS, TCP echo, UDP, TLS to github.com, HTTP GET, SoftAP) | `apps/ble_test` (scan list, connect to a phone app, GATT server with notify) |

## Open questions

1. Can an app save a network it joined, or only join for one run (as above)?
2. SoftAP in NET v1, or left for later?
3. Is BLE worth its RAM? NimBLE is loaded only while a BLE app runs, but the esp_hosted BT path also needs some memory at boot.
4. BLE HID gamepads: as a launcher input source (BTHD → INPT) rather than per app? They'd then work in every emulator.
