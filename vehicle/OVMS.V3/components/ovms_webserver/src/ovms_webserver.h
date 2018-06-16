/*
;    Project:       Open Vehicle Monitor System
;    Date:          14th March 2017
;
;    Changes:
;    1.0  Initial release
;
;    (C) 2011       Michael Stegen / Stegen Electronics
;    (C) 2011-2017  Mark Webb-Johnson
;    (C) 2011        Sonny Chen @ EPRO/DX
;    (C) 2018       Michael Balzer
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.
*/

#ifndef __WEBSERVER_H__
#define __WEBSERVER_H__

#include <forward_list>
#include <iterator>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "ovms_events.h"
#include "ovms_metrics.h"
#include "ovms_config.h"
#include "ovms_command.h"
#include "ovms_shell.h"
#include "ovms_netmanager.h"

#define OVMS_GLOBAL_AUTH_FILE     "/store/.htpasswd"

#define SESSION_COOKIE_NAME       "ovms_session"
#define SESSION_TTL               3600
#define SESSION_CHECK_INTERVAL    60
#define NUM_SESSIONS              5

#define XFER_CHUNK_SIZE           1024

#define WEBSRV_USE_MG_BROADCAST   0  // Note: mg_broadcast() not working reliably yet, do not enable for production!


struct user_session {
  uint64_t id;
  time_t last_used;
  //char* username;
};


/**
 * PageContext: execution context of a URI/page handler call providing
 *  access to the HTTP context and utilities to generate HTML output.
 */

struct PageContext : public ExternalRamAllocated
{
  mg_connection *nc;
  http_message *hm;
  user_session *session;
  std::string method;
  std::string uri;
  
  // utils:
  std::string getvar(const char* name, size_t maxlen=200);
  std::string encode_html(const char* text);
  std::string encode_html(const std::string text);
  
  // output:
  void error(int code, const char* text);
  void head(int code, const char* headers=NULL);
  void print(const std::string text);
  void print(const char* text);
  void printf(const char *fmt, ...);
  void done();
  void panel_start(const char* type, const char* title);
  void panel_end(const char* footer="");
  void form_start(const char* action);
  void form_end();
  void fieldset_start(const char* title, const char* css_class=NULL);
  void fieldset_end();
  void hr();
  void input(const char* type, const char* label, const char* name, const char* value,
    const char* placeholder=NULL, const char* helptext=NULL, const char* moreattrs=NULL,
    const char* unit=NULL);
  void input_text(const char* label, const char* name, const char* value,
    const char* placeholder=NULL, const char* helptext=NULL, const char* moreattrs=NULL);
  void input_password(const char* label, const char* name, const char* value,
    const char* placeholder=NULL, const char* helptext=NULL, const char* moreattrs=NULL);
  void input_select_start(const char* label, const char* name);
  void input_select_option(const char* label, const char* value, bool selected);
  void input_select_end(const char* helptext=NULL);
  void input_radio_start(const char* label, const char* name);
  void input_radio_option(const char* name, const char* label, const char* value, bool selected);
  void input_radio_end(const char* helptext=NULL);
  void input_radiobtn_start(const char* label, const char* name);
  void input_radiobtn_option(const char* name, const char* label, const char* value, bool selected);
  void input_radiobtn_end(const char* helptext=NULL);
  void input_checkbox(const char* label, const char* name, bool value, const char* helptext=NULL);
  void input_slider(const char* label, const char* name, int size, const char* unit,
    int enabled, double value, double defval, double min, double max, double step=1,
    const char* helptext=NULL);
  void input_button(const char* type, const char* label, const char* name=NULL, const char* value=NULL);
  void input_info(const char* label, const char* text);
  void alert(const char* type, const char* text);
};

typedef struct PageContext PageContext_t;


/**
 * PageEntry: HTTP URI page handler entry for OvmsWebServer.
 * 
 * Created by OvmsWebServer::RegisterPage(). The registered handler will be called
 *  with both PageEntry and PageContext, so one handler can serve multiple
 *  URIs or patterns.
 */

typedef enum {
  PageMenu_None,
  PageMenu_Main,              // → main menu
  PageMenu_Config,            // → config menu
  PageMenu_Vehicle,           // → vehicle menu
} PageMenu_t;

typedef enum {
  PageAuth_None,              // public
  PageAuth_Cookie,            // use auth cookie
  PageAuth_File,              // use htaccess file(s) (digest auth)
} PageAuth_t;

struct PageEntry;
typedef struct PageEntry PageEntry_t;
typedef void (*PageHandler_t)(PageEntry_t& p, PageContext_t& c);

struct PageEntry
{
  const char* uri;
  const char* label;
  PageHandler_t handler;
  PageMenu_t menu;
  PageAuth_t auth;
  
  PageEntry(const char* _uri, const char* _label, PageHandler_t _handler, PageMenu_t _menu=PageMenu_None, PageAuth_t _auth=PageAuth_None)
  {
    uri = _uri;
    label = _label;
    handler = _handler;
    menu = _menu;
    auth = _auth;
  }
  
  void Serve(PageContext_t& c);
};

typedef std::forward_list<PageEntry*> PageMap_t;


/**
 * MgHandler is the base mongoose connection handler interface class
 *  to implement stateful connections.
 * 
 * An MgHandler instance automatically attaches itself to (and detaches from)
 *  the mg_connection via the user_data field.
 * The HandleEvent() method will be called prior to the framework handler.
 */
class MgHandler : public ExternalRamAllocated
{
  public:
    MgHandler(mg_connection* nc)
    {
      m_nc = nc;
      if (m_nc)
        m_nc->user_data = this;
    }
    virtual ~MgHandler()
    {
      if (m_nc)
        m_nc->user_data = NULL;
    }
  
  public:
    virtual int HandleEvent(int ev, void* p) = 0;
    void RequestPoll();
    static void HandlePoll(mg_connection* nc, int ev, void* p);
  
  public:
    mg_connection*            m_nc;
};


/**
 * HttpDataSender transmits a memory area (ROM/RAM) in HTTP chunks of XFER_CHUNK_SIZE size.
 */
class HttpDataSender : public MgHandler
{
  public:
    HttpDataSender(mg_connection* nc, const uint8_t* data, size_t size, bool keepalive=true);
    ~HttpDataSender();
  
  public:
    int HandleEvent(int ev, void* p);
  
  public:
    const uint8_t*            m_data;             // pointer to data
    size_t                    m_size;             // size of data to send
    size_t                    m_sent;             // size sent up to now
    bool                      m_keepalive;        // false = close connection when done
};


/**
 * HttpStringSender transmits a std::string in HTTP chunks of XFER_CHUNK_SIZE size.
 * Note: the string is deleted after transmission.
 */
class HttpStringSender : public MgHandler
{
  public:
    HttpStringSender(mg_connection* nc, std::string* msg, bool keepalive=true);
    ~HttpStringSender();
  
  public:
    int HandleEvent(int ev, void* p);
  
  public:
    std::string*              m_msg;              // pointer to data
    size_t                    m_sent;             // size sent up to now
    bool                      m_keepalive;        // false = close connection when done
};


/**
 * WebSocketHandler transmits JSON data in chunks to the WebSocket client
 *  and coordinates transmits initiated from other contexts (i.e. events).
 * 
 * On creation it will do a full update of all metrics.
 * Later on, it receives TX jobs through the queue.
 */

enum WebSocketTxJobType
{
  WSTX_None,
  WSTX_Event,                 // payload: event
  WSTX_MetricsAll,            // payload: -
  WSTX_MetricsUpdate,         // payload: -
  WSTX_Config,                // payload: config (todo)
};

struct WebSocketTxJob
{
  WebSocketTxJobType          type;
  union
  {
    char*                     event;
    OvmsConfigParam*          config;
  };
};

class WebSocketHandler : public MgHandler
{
  public:
    WebSocketHandler(mg_connection* nc, size_t modifier);
    ~WebSocketHandler();
  
  public:
    bool Lock(TickType_t xTicksToWait);
    void Unlock();
    bool AddTxJob(WebSocketTxJob job, bool init_tx=true);
    void FreeTxJob(WebSocketTxJob &job);
    bool GetNextTxJob();
    void InitTx();
    void ContinueTx();
    void ProcessTxJob();
    int HandleEvent(int ev, void* p);
  
  public:
    size_t                    m_modifier;         // "our" metrics modifier
    QueueHandle_t             m_jobqueue;
    int                       m_jobqueue_overflow;
    SemaphoreHandle_t         m_mutex;
    WebSocketTxJob            m_job;
    int                       m_sent;
    int                       m_ack;
};

struct WebSocketSlot
{
  WebSocketHandler*   handler;
  size_t              modifier;
};

typedef std::vector<WebSocketSlot> WebSocketSlots;


/**
 * HttpCommandStream: execute command, stream output to HTTP connection
 */

class HttpCommandStream : public OvmsShell, public MgHandler
{
  public:
    HttpCommandStream(mg_connection* nc, std::string command, int verbosity=COMMAND_RESULT_NORMAL);
    ~HttpCommandStream();
  
  public:
    void ProcessQueue();
    int HandleEvent(int ev, void* p);
    static void CommandTask(void* object);
  
  public:
    std::string               m_command;
    TaskHandle_t              m_cmdtask;
    QueueHandle_t             m_writequeue;
    bool                      m_done;
    size_t                    m_sent;
    int                       m_ack;
  
  public:
    void Initialize(bool print);
    virtual bool IsInteractive() { return false; }
    char** GetCompletion(OvmsCommandMap& children, const char* token) { return NULL; }
    int puts(const char* s);
    int printf(const char* fmt, ...);
    ssize_t write(const void *buf, size_t nbyte);
    void Log(LogBuffers* message);
};



/**
 * OvmsWebServer: main web framework (static instance: MyWebServer)
 * 
 * Register custom page handlers through the RegisterPage() API.
 */

class OvmsWebServer : public ExternalRamAllocated
{
  public:
    OvmsWebServer();
    ~OvmsWebServer();

  public:
    static void EventHandler(mg_connection *nc, int ev, void *p);
    void NetManInit(std::string event, void* data);
    void NetManStop(std::string event, void* data);
    void ConfigChanged(std::string event, void* data);
    void UpdateGlobalAuthFile();
    static const std::string MakeDigestAuth(const char* realm, const char* username, const char* password);
    static const std::string ExecuteCommand(const std::string command, int verbosity=COMMAND_RESULT_NORMAL);
    static void WebsocketBroadcast(const std::string msg);
    void EventListener(std::string event, void* data);
    void BroadcastMetrics(bool update_all);
    static void UpdateTicker(TimerHandle_t timer);

  public:
    void RegisterPage(const char* uri, const char* label, PageHandler_t handler,
      PageMenu_t menu=PageMenu_None, PageAuth_t auth=PageAuth_None);
    void DeregisterPage(const char* uri);
    PageEntry* FindPage(const char* uri);
  
  public:
    user_session* CreateSession(const http_message *hm);
    void DestroySession(user_session *s);
    user_session* GetSession(http_message *hm);
    void CheckSessions(void);
    static bool CheckLogin(std::string username, std::string password);
  
  public:
    WebSocketHandler* CreateWebSocketHandler(mg_connection* nc);
    void DestroyWebSocketHandler(WebSocketHandler* handler);
  
  public:
    static std::string CreateMenu(PageContext_t& c);
    static void OutputHome(PageEntry_t& p, PageContext_t& c);
    static void HandleRoot(PageEntry_t& p, PageContext_t& c);
    static void HandleAsset(PageEntry_t& p, PageContext_t& c);
    static void HandleMenu(PageEntry_t& p, PageContext_t& c);
    static void HandleHome(PageEntry_t& p, PageContext_t& c);
    static void HandleLogin(PageEntry_t& p, PageContext_t& c);
    static void HandleLogout(PageEntry_t& p, PageContext_t& c);
    static void OutputReboot(PageEntry_t& p, PageContext_t& c);
    static void OutputReconnect(PageEntry_t& p, PageContext_t& c, const char* info=NULL);
  
  public:
    static void HandleStatus(PageEntry_t& p, PageContext_t& c);
    static void HandleCommand(PageEntry_t& p, PageContext_t& c);
    static void HandleShell(PageEntry_t& p, PageContext_t& c);
    static void HandleDashboard(PageEntry_t& p, PageContext_t& c);
    static void HandleCfgPassword(PageEntry_t& p, PageContext_t& c);
    static void HandleCfgVehicle(PageEntry_t& p, PageContext_t& c);
    static void HandleCfgModem(PageEntry_t& p, PageContext_t& c);
    static void HandleCfgServerV2(PageEntry_t& p, PageContext_t& c);
    static void HandleCfgServerV3(PageEntry_t& p, PageContext_t& c);
    static void HandleCfgWebServer(PageEntry_t& p, PageContext_t& c);
    static void HandleCfgWifi(PageEntry_t& p, PageContext_t& c);
    static void OutputWifiTable(PageEntry_t& p, PageContext_t& c, const std::string prefix, const std::string paramname,
      const std::string autostart_ssid);
    static void UpdateWifiTable(PageEntry_t& p, PageContext_t& c, const std::string prefix, const std::string paramname,
      std::string& warn, std::string& error, int pass_minlen);
    static void HandleCfgAutoInit(PageEntry_t& p, PageContext_t& c);
    static void HandleCfgFirmware(PageEntry_t& p, PageContext_t& c);
    static void HandleCfgLogging(PageEntry_t& p, PageContext_t& c);
    static void HandleCfgLocations(PageEntry_t& p, PageContext_t& c);

  public:
    void CfgInitStartup();
    static void HandleCfgInit(PageEntry_t& p, PageContext_t& c);
    static std::string CfgInitSetStep(std::string step, int timeout=0);
    void CfgInitTicker();
    std::string CfgInit1(PageEntry_t& p, PageContext_t& c, std::string step);
    std::string CfgInit2(PageEntry_t& p, PageContext_t& c, std::string step);
    std::string CfgInit3(PageEntry_t& p, PageContext_t& c, std::string step);
    std::string CfgInit4(PageEntry_t& p, PageContext_t& c, std::string step);
    std::string CfgInit5(PageEntry_t& p, PageContext_t& c, std::string step);


  public:
    bool                      m_running;

#if MG_ENABLE_FILESYSTEM
    bool                      m_file_enable;
    mg_serve_http_opts        m_file_opts;
#endif //MG_ENABLE_FILESYSTEM

    PageMap_t                 m_pagemap;

    user_session              m_sessions[NUM_SESSIONS];
    
    size_t                    m_client_cnt;                 // number of active WebSocket clients
    SemaphoreHandle_t         m_client_mutex;
    WebSocketSlots            m_client_slots;
    TimerHandle_t             m_update_ticker;
    
    int                       m_init_timeout;
};

extern OvmsWebServer MyWebServer;


/**
 * DashboardConfig:
 */
struct DashboardConfig
{
  std::string gaugeset1;
};

#endif //#ifndef __WEBSERVER_H__
