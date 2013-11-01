/* Copyright (C) 2013 gsfan, MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/** @file
 * @brief Gainspan wi-fi module library for mbed
 * GS1011MIC, GS1011MIP, GainSpan WiFi Breakout, etc.
 */

#include "Arduino.h"
#include "pgmStrToRAM.h"
#include "GSwifi.h"
#include "MemoryFree.h"
#include "convert.h"
#include "ringbuffer.h"
#include "version.h"

#define DOMAIN "wifi-morse-setup.herokuapp.com"

#define RESPONSE_LINES_ENDED -1

#define NEXT_TOKEN_CID    0
#define NEXT_TOKEN_IP     1
#define NEXT_TOKEN_PORT   2
#define NEXT_TOKEN_LENGTH 3
#define NEXT_TOKEN_DATA   4

#define CID_UNDEFINED     0xFF

#define ESCAPE           0x1B

GSwifi::GSwifi( HardwareSerial *serial ) :
    _serial(serial)
{
    _buf_cmd          = &ringbuffer;
    ring_init( _buf_cmd );
    _route_count      = 0;
    newest_message_id = 0;
    clientRequest.cid = CID_UNDEFINED;
}

int8_t GSwifi::setup(GSEventHandler onDisconnect, GSEventHandler onReset) {
    char cmd[GS_CMD_SIZE];

    onDisconnect_ = onDisconnect;
    onReset_      = onReset;

    reset();

    _serial->begin(9600);

    command(PB("AT",1), GSCOMMANDMODE_NORMAL);

    // disable echo
    command(PB("ATE0",1), GSCOMMANDMODE_NORMAL);

    // faster baud rate
    // TODO enable when ready
    // setBaud(115200);

    sprintf(cmd, P("AT+HTTPCONF=20,IRKit/%s"), version);
    command(cmd, GSCOMMANDMODE_NORMAL);

    sprintf(cmd, P("AT+HTTPCONF=11,%s"), DOMAIN);
    command(cmd, GSCOMMANDMODE_NORMAL);

    // finding an open socket for our server uses program space
    sprintf(cmd, P("AT+HTTPCONF=3,close"));
    command(cmd, GSCOMMANDMODE_NORMAL);

    command(PB("AT+PSPOLLINTRL=0",1), GSCOMMANDMODE_NORMAL);
    if (did_timeout_) {
        return -1;
    }

    return 0;
}

int8_t GSwifi::close (uint8_t cid) {
    char *cmd = PB("AT+NCLOSE=0", 1);
    cmd[ 10 ]  = cid + '0';

    command(cmd, GSCOMMANDMODE_NORMAL);
    if (did_timeout_) {
        return -1;
    }
    return 0;
}

void GSwifi::reset () {
    _joined         = false;
    _listening      = false;
    _power_status   = GSPOWERSTATUS_READY;
    _escape         = false;
    resetResponse(GSCOMMANDMODE_NONE);
    _gs_mode        = GSMODE_COMMAND;
    _dhcp           = false;
    ring_clear(_buf_cmd);
    serverRequest.cid = CID_UNDEFINED;
    clientRequest.cid = CID_UNDEFINED;
}

void GSwifi::loop() {
    checkActivity( 0 );
}

// received a character from UART
void GSwifi::parseByte(uint8_t dat) {
    Serial.print(dat, HEX);
    if (dat > 0x0D) {
        Serial.print(P(" "));
        Serial.write(dat);
    }
    Serial.println();

    static int len;
    static int next_token; // split each byte into tokens (cid,ip,port,length,data)
    static char tmp[20];
    static uint8_t continous_newlines = 0;

    // true  : data from gswifi is response to request from gswifi
    // false : data from gswifi is request from other client
    static bool is_response = 0;

    if (_gs_mode == GSMODE_COMMAND) {
        if (_escape) {
            // esc
            switch (dat) {
            case 'O':
                Serial.println(P("ok"));
                _gs_ok      = true;
                break;
            case 'F':
                Serial.println(P("failure"));
                _gs_failure = true;
                break;
            case 'Z':
            case 'H':
                Serial.println(P("GSMODE_DATA_RX_BULK"));
                _gs_mode   = GSMODE_DATA_RX_BULK;
                next_token = NEXT_TOKEN_CID;
                break;
            default:
                Serial.print(P("!!!E1 ")); Serial.println(dat,HEX);
                break;
            }
            _escape = false;
        }
        else {
            if (dat == 0x1b) {
                _escape = true;
            }
            else if (dat == '\n') {
                // end of line
                parseLine();
            }
            else if (dat != '\r') {
                // command
                if ( ! ring_isfull(_buf_cmd) ) {
                    ring_put(_buf_cmd, dat);
                }
                else {
                    Serial.println(P("!!!E2"));
                }
            }
        }
    }
    else if (_gs_mode == GSMODE_DATA_RX_BULK) {
        if (next_token == NEXT_TOKEN_CID) {
            // dat is cid
            uint8_t cid = x2i(dat);
            next_token  = NEXT_TOKEN_LENGTH;
            len         = 0;
            if (clientRequest.cid == cid) {
                // following data is response to our request from gswifi
                is_response = true;
            }
            else {
                // following data is request from other client in same wifi
                is_response = false;
            }
        }
        else if (next_token == NEXT_TOKEN_LENGTH) {
            // Data Length is 4 ascii char represents decimal value i.e. 1400 byte (0x31 0x34 0x30 0x30)
            tmp[ len ++ ] = dat;
            if (len >= 4) {
                tmp[ len ] = 0;
                len        = atoi(tmp); // length of data
                next_token = NEXT_TOKEN_DATA;

                serverRequest.state = GSREQUESTSTATE_HEAD1;
                ring_clear( _buf_cmd ); // reuse _buf_cmd to store HTTP request

                Serial.print(P("bulk length:")); Serial.println(tmp);
            }
        }
        else if (next_token == NEXT_TOKEN_DATA) {
            len --;

            if (is_response) {
                switch (clientRequest.state) {
                case GSRESPONSESTATE_STATUSLINE:
                    if (dat != '\n') {
                        ring_put( _buf_cmd, dat );
                    }
                    else {
                        char status_code[4];
                        status_code[ 3 ] = 0;
                        int8_t count = ring_get( _buf_cmd, status_code, 3 );
                        if (count != 3) {
                            // protocol error
                            // we should receive something like: "200 OK", "401 Unauthorized"
                            // TODO handle this
                            clientRequest.status_code = 999;
                            clientRequest.state       = GSRESPONSESTATE_ERROR;
                            break;
                        }
                        ring_clear(_buf_cmd);
                        clientRequest.status_code = atoi(status_code);
                        clientRequest.state       = GSRESPONSESTATE_BODY;
                    }
                    break;
                case GSRESPONSESTATE_BODY:
                    if (ring_isfull(_buf_cmd)) {
                        dispatchResponseHandler();
                    }
                    ring_put(_buf_cmd, dat);
                    break;
                case GSRESPONSESTATE_ERROR:
                case GSRESPONSESTATE_RECEIVED:
                default:
                    break;
                }

                if (len == 0) {
                    Serial.println(P("len==0"));

                    _escape             = false;
                    _gs_mode            = GSMODE_COMMAND;
                    clientRequest.state = GSRESPONSESTATE_RECEIVED;
                    clientRequest.cid   = CID_UNDEFINED;
                    dispatchResponseHandler();
                }
                return;
            }

            switch (serverRequest.state) {
            case GSREQUESTSTATE_HEAD1:
                if (dat != '\n') {
                    ring_put( _buf_cmd, dat );
                }
                else {
                    // end of request line
                    char    method[8];
                    uint8_t method_size = 7;
                    char    path[ GS_MAX_PATH_LENGTH + 1 ];
                    uint8_t path_size   = GS_MAX_PATH_LENGTH;
                    int8_t  result      = parseRequestLine((char*)method, method_size);
                    if ( result == 0 ) {
                        Serial.print(P("method:")); Serial.println(method);
                        result = parseRequestLine((char*)path, path_size);
                    }
                    if ( result != 0 ) {
                        // couldn't detect method or path
                        serverRequest.state      = GSREQUESTSTATE_ERROR;
                        serverRequest.error_code = 400;
                        ring_clear(_buf_cmd);
                        Serial.println(P("error400"));
                        break;
                    }
                    Serial.print(P("path:")); Serial.println(path);
                    GSMETHOD gsmethod = x2method(method);

                    int8_t routeid = router(gsmethod, path);
                    if ( routeid < 0 ) {
                        serverRequest.state      = GSREQUESTSTATE_ERROR;
                        serverRequest.error_code = 404;
                        ring_clear(_buf_cmd);
                        Serial.println(P("error404"));
                        break;
                    }
                    serverRequest.routeid = routeid;
                    serverRequest.state   = GSREQUESTSTATE_HEAD2;
                    continous_newlines    = 0;
                    Serial.println(P("next: head2"));
                }
                break;
            case GSREQUESTSTATE_HEAD2:
                if (dat == '\n') {
                    continous_newlines ++;
                }
                else if (dat == '\r') {
                    // preserve
                }
                else {
                    continous_newlines = 0;
                }
                if (continous_newlines == 2) {
                    // if detected double \n, switch to body mode
                    serverRequest.state = GSREQUESTSTATE_BODY;
                    ring_clear(_buf_cmd);
                    Serial.println(P("next: body"));
                }
                break;
            case GSREQUESTSTATE_BODY:
                if (ring_isfull(_buf_cmd)) {
                    Serial.println(P("full"));
                    dispatchRequestHandler(); // POST, user callback should write()
                }
                ring_put(_buf_cmd, dat);
                break;
            case GSREQUESTSTATE_ERROR:
                // skip until received whole request
                break;
            case GSREQUESTSTATE_RECEIVED:
            default:
                break;
            }

            if (len == 0) {
                Serial.println(P("len==0"));

                _escape  = false;
                _gs_mode = GSMODE_COMMAND;
                if ( serverRequest.state == GSREQUESTSTATE_ERROR ) {
                    writeHead( serverRequest.error_code );
                    end();
                }
                else {
                    serverRequest.state = GSREQUESTSTATE_RECEIVED;
                    dispatchRequestHandler(); // user callback should write() and end()
                }
                serverRequest.cid = CID_UNDEFINED;
            }
        } // (next_token == NEXT_TOKEN_DATA)
    } // (_gs_mode == GSMODE_DATA_RX_BULK)
}

int8_t GSwifi::parseRequestLine (char *token, uint8_t token_size) {
    uint8_t i;
    for ( i = 0; i <= token_size; i++ ) {
        if (ring_isempty( _buf_cmd )) {
            return -1; // space didn't appear
        }
        ring_get( _buf_cmd, token+i, 1 );
        if (token[i] == ' ') {
            token[i] = '\0';
            break;
        }
    }
    if ( i == token_size + 1 ) {
        return -1; // couldnt detect token
    }
    return 0;
}

int8_t GSwifi::router (GSMETHOD method, const char *path) {
    if (method == GSMETHOD_UNKNOWN) {
        return -1;
    }

    uint8_t i;
    for (i = 0; i < _route_count; i ++) {
        if ((method == _routes[i].method) &&
            (strncmp(path, _routes[i].path, GS_MAX_PATH_LENGTH) == 0)) {
            Serial.print(P("router matched: ")); Serial.println(i);
            return i;
        }
    }
    return -1;
}

int8_t GSwifi::registerRoute (GSwifi::GSMETHOD method, const char *path) {
    if ( _route_count >= GS_MAX_ROUTES ) {
        return -1;
    }
    _routes[ _route_count ].method = method;
    strncpy(_routes[ _route_count ].path, path, sizeof(_routes[_route_count].path));
    _route_count ++;
}

void GSwifi::setRequestHandler (GSEventHandler handler) {
    _requestHandler = handler;
}

int8_t GSwifi::dispatchRequestHandler () {
    return _requestHandler();
}

int8_t GSwifi::dispatchResponseHandler () {
    return _responseHandler();
}

int8_t GSwifi::writeHead (uint16_t status_code) {
    char *cmd = PB("S0",1);
    cmd[ 1 ]  = serverRequest.cid + '0';

    escape( cmd );
    if (did_timeout_) {
        return -1;
    }

    _serial->print(P("HTTP/1.0 "));
    char *msg;
    switch (status_code) {
    case 200:
        msg = P("200 OK");
        break;
    case 400:
        msg = P("400 Bad Request");
        break;
    case 404:
        msg = P("404 Not Found");
        break;
    case 500:
    default:
        msg = P("500 Internal Server Error");
        break;
    }

    _serial->println(msg);
    _serial->println(P("Content-Type: text/plain\r\n")); // TODO json
}

void GSwifi::write (const char *data) {
    _serial->print(data);
}

void GSwifi::write (const uint8_t data) {
    _serial->print(data);
}

void GSwifi::write (const uint16_t data) {
    _serial->print(data);
}

int8_t GSwifi::end () {
    escape( PB("E",1) );
    if (did_timeout_) {
        // close anyway
    }

    return close( serverRequest.cid );
}

GSwifi::GSMETHOD GSwifi::x2method(const char *method) {
    if (strncmp(method, P("GET"), 3) == 0) {
        return GSMETHOD_GET;
    }
    else if (strncmp(method, P("POST"), 4) == 0) {
        return GSMETHOD_POST;
    }
    return GSMETHOD_UNKNOWN;
}

void GSwifi::parseLine () {
    uint8_t i;
    char buf[GS_CMD_SIZE];

    while (! ring_isempty(_buf_cmd)) {
        // received "\n"
        i = 0;
        while ( (! ring_isempty(_buf_cmd)) &&
                (i < sizeof(buf) - 1) ) {
            ring_get( _buf_cmd, &buf[i], 1 );
            if (buf[i] == '\n') {
                break;
            }
            i ++;
        }
        if (i == 0) continue;
        buf[i] = 0;

        if ( (_gs_mode == GSMODE_COMMAND) &&
             (_gs_commandmode != GSCOMMANDMODE_NONE) ) {
            parseCmdResponse(buf);
        }

        if (strncmp(buf, P("CONNECT "), 8) == 0 && buf[8] >= '0' && buf[8] <= 'F' && buf[9] != 0) {
            // connect from client
            // CONNECT 0 1 192.168.2.1 63632
            // 1st cid is our http server's, should be 0
            // 2nd cid is for client
            // next line will be "[ESC]Z10140GET / ..."

            Serial.println(buf);
            uint8_t cid = x2i(buf[10]); // 2nd cid = HTTP client cid

            if ( (serverRequest.cid != CID_UNDEFINED) &&
                 (serverRequest.cid != cid) ){
                // if a connection is left over, close it before hand
                close( serverRequest.cid );
            }

            serverRequest.cid    = cid;
            serverRequest.state  = GSREQUESTSTATE_PREPARE;
            serverRequest.length = 0;

            // ignore client's IP and port
        }
        else if (strncmp(buf, P("DISCONNECT "), 11) == 0) {
            uint8_t cid = x2i(buf[11]);
            Serial.print(P("disconnect ")); Serial.println(cid);
            if (cid == clientRequest.cid) {
                clientRequest.cid = CID_UNDEFINED;
            }
            else if (cid == serverRequest.cid) {
                serverRequest.cid = CID_UNDEFINED;
            }
        }
        else if (strncmp(buf, P("DISASSOCIATED"), 13) == 0 ||
                 strncmp(buf, P("Disassociated"), 13) == 0 ||
                 strncmp(buf, P("Disassociation Event"), 20) == 0 ) {
            reset();
            onDisconnect_();
        }
        else if (strncmp(buf, P("UnExpected Warm Boot"), 20) == 0 ||
                 strncmp(buf, P("APP Reset-APP SW Reset"), 22) == 0 ||
                 strncmp(buf, P("APP Reset-Wlan Except"), 21) == 0 ) {
            Serial.println(P("disassociate"));
            reset();
            onReset_();
        }
        else if (strncmp(buf, P("Out of StandBy-Timer"), 20) == 0 ||
                 strncmp(buf, P("Out of StandBy-Alarm"), 20) == 0) {
            if (_power_status == GSPOWERSTATUS_STANDBY) {
                _power_status = GSPOWERSTATUS_WAKEUP;
            }
        }
        else if (strncmp(buf, P("Out of Deep Sleep"), 17) == 0 ) {
            if (_power_status == GSPOWERSTATUS_DEEPSLEEP) {
                _power_status = GSPOWERSTATUS_READY;
            }
        }
        // Serial.print(P("status: ")); Serial.println(_power_status, HEX);
    }
}

void GSwifi::parseCmdResponse (char *buf) {
    Serial.print(P("parseCmd: ")); Serial.println(buf);

    if (strcmp(buf, P("OK")) == 0) {
        _gs_ok = true;
    }
    else if (strncmp(buf, P("ERROR"), 5) == 0) {
        _gs_failure = true;
    }

    switch(_gs_commandmode) {
    case GSCOMMANDMODE_NORMAL:
        _gs_response_lines = RESPONSE_LINES_ENDED;
        break;
    case GSCOMMANDMODE_CONNECT:
        if (strncmp(buf, P("CONNECT "), 8) == 0 && buf[9] == 0) {
            // server started listening
            _gs_response_lines = RESPONSE_LINES_ENDED;
        }
        break;
    case GSCOMMANDMODE_DHCP:
        if (_gs_response_lines == 0 && strstr(buf, P("SubNet")) && strstr(buf, P("Gateway"))) {
            _gs_response_lines ++;
        } else
        if (_gs_response_lines == 1) {
            // int ip1, ip2, ip3, ip4;
            // char *tmp = buf + 1;
            // sscanf(tmp, P("%d.%d.%d.%d"), &ip1, &ip2, &ip3, &ip4);
            // _ipaddr = IpAddr(ip1, ip2, ip3, ip4);
            // tmp = strstr(tmp, ":") + 2;
            // sscanf(tmp, P("%d.%d.%d.%d"), &ip1, &ip2, &ip3, &ip4);
            // _netmask = IpAddr(ip1, ip2, ip3, ip4);
            // tmp = strstr(tmp, ":") + 2;
            // sscanf(tmp, P("%d.%d.%d.%d"), &ip1, &ip2, &ip3, &ip4);
            // _gateway = IpAddr(ip1, ip2, ip3, ip4);
            _gs_response_lines = RESPONSE_LINES_ENDED;
        }
        break;
    case GSCOMMANDMODE_DNSLOOKUP:
        if (strncmp(buf, P("IP:"), 3) == 0) {
            // int ip1, ip2, ip3, ip4;
            // sscanf(&buf[3], P("%d.%d.%d.%d"), &ip1, &ip2, &ip3, &ip4);
            // _resolv = IpAddr(ip1, ip2, ip3, ip4);
            _gs_response_lines = RESPONSE_LINES_ENDED;
        }
        break;
    case GSCOMMANDMODE_HTTP:
        if (_gs_response_lines == 0 && strstr(buf, P("IP:"))) {
            // ignore IP
            _gs_response_lines ++;
        }
        else if (_gs_response_lines == 1) {
            if (buf[0] >= '0' && buf[0] <= 'F' && buf[1] == 0) {
                uint8_t cid = x2i(buf[0]);

                if ( (clientRequest.cid != CID_UNDEFINED) &&
                     (clientRequest.cid != cid) ){
                    // if a connection is left over, close it before hand
                    close( clientRequest.cid );
                }

                clientRequest.cid   = cid;
                clientRequest.state = GSRESPONSESTATE_STATUSLINE;
            }
            _gs_response_lines = RESPONSE_LINES_ENDED;
        }
        break;
    case GSCOMMANDMODE_STATUS:
        if (_gs_response_lines == 0 && strncmp(buf, P("NOT ASSOCIATED"), 14) == 0) {
            _joined    = false;
            _listening = false;
            // for (int i = 0; i < 16; i ++) {
            //     _gs_sock[i].connect = false;
            // }
            _gs_response_lines = RESPONSE_LINES_ENDED;
        }
        if (_gs_response_lines == 0 && strncmp(buf, P("MODE:"), 5) == 0) {
            _gs_response_lines ++;
        }
        else if (_gs_response_lines == 1 && strncmp(buf, P("BSSID:"), 6) == 0) {
            _gs_response_lines = RESPONSE_LINES_ENDED;
        }
        break;
    }

    return;
}

void GSwifi::command (const char *cmd, GSCOMMANDMODE res, uint32_t timeout) {
    Serial.print(P("c> "));

    resetResponse(res);

    _serial->println(cmd);

    Serial.println(cmd);

    setBusy(true);
    waitResponse(timeout);
}

void GSwifi::escape (const char *cmd, uint32_t timeout) {
    Serial.print(P("e> "));

    resetResponse(GSCOMMANDMODE_NONE);

    _serial->write( 0x1B );
    _serial->print(cmd); // without ln

    Serial.println(cmd);

    setBusy(true);
    waitResponse(timeout);
}

void GSwifi::resetResponse (GSCOMMANDMODE res) {
    _gs_ok             = false;
    _gs_failure        = false;
    _gs_response_lines = 0;
    _gs_commandmode    = res;
}

bool GSwifi::setBusy(bool busy) {
    if (busy) {
        timeout_start_ = millis();
        did_timeout_   = false;
        // if (onBusy) onBusy();
    } else {
        // lastError = false;
        // if (onIdle) onIdle();
    }
    return busy_ = busy;
}

uint8_t GSwifi::checkActivity(uint32_t timeout_ms) {
    while ( _serial->available() &&
            ( (timeout_ms == 0) ||
              millis() - timeout_start_ < timeout_ms ) ) {

        parseByte( _serial->read() );

        if ( (_gs_ok || _gs_failure) &&
             (_gs_response_lines == RESPONSE_LINES_ENDED || _gs_commandmode == GSCOMMANDMODE_NONE) ) {
            _gs_commandmode = GSCOMMANDMODE_NONE;
            setBusy(false);
            break;
        }
    }

    if ( (timeout_ms > 0) &&
         busy_ &&
         (millis() - timeout_start_ >= timeout_ms) ) {
        Serial.println(P("!!! did timeout !!!"));
        did_timeout_ = true;
        setBusy(false);
    }

    return busy_;
}

void GSwifi::waitResponse (uint32_t ms) {
    while ( checkActivity(ms) ) {
    }
}

int GSwifi::join (GSSECURITY sec, const char *ssid, const char *pass, int dhcp, char *name) {
    char cmd[GS_CMD_SIZE];

    if (_joined || _power_status != GSPOWERSTATUS_READY) return -1;

    command(PB("AT+BDATA=1",1), GSCOMMANDMODE_NORMAL);
    if (did_timeout_) {
        return -1;
    }

    disconnect();

    // infrastructure mode
    command(PB("AT+WM=0",1), GSCOMMANDMODE_NORMAL);
    if (did_timeout_) {
        return -1;
    }

    // dhcp
    if (dhcp) {
        command(PB("AT+NDHCP=1",1), GSCOMMANDMODE_NORMAL);
    } else {
        command(PB("AT+NDHCP=0",1), GSCOMMANDMODE_NORMAL);
    }
    if (did_timeout_) {
        return -1;
    }

    switch (sec) {
    case GSSECURITY_NONE:
    case GSSECURITY_OPEN:
    case GSSECURITY_WEP:
        sprintf(cmd, P("AT+WAUTH=%d"), sec);
        command(cmd, GSCOMMANDMODE_NORMAL);
        if (sec != GSSECURITY_NONE) {
            sprintf(cmd, P("AT+WWEP1=%s"), pass);
            command(cmd, GSCOMMANDMODE_NORMAL);
            // wait_ms(100);
        }
        sprintf(cmd, P("AT+WA=%s"), ssid);
        command(cmd, GSCOMMANDMODE_DHCP, GS_TIMEOUT2);
        break;
    case GSSECURITY_WPA_PSK:
        command(PB("AT+WAUTH=0",1), GSCOMMANDMODE_NORMAL);

        sprintf(cmd, P("AT+WWPA=%s"), pass);
        command(cmd, GSCOMMANDMODE_NORMAL, GS_TIMEOUT2);

        sprintf(cmd, P("AT+WA=%s"), ssid);
        command(cmd, GSCOMMANDMODE_DHCP, GS_TIMEOUT2);
        break;
    case GSSECURITY_WPA2_PSK:
        command(PB("AT+WAUTH=0",1), GSCOMMANDMODE_NORMAL);
        sprintf(cmd, P("AT+WPAPSK=%s,%s"), ssid, pass);
        command(cmd, GSCOMMANDMODE_NORMAL, GS_TIMEOUT2);

        sprintf(cmd, P("AT+WA=%s"), ssid);
        command(cmd, GSCOMMANDMODE_DHCP, GS_TIMEOUT2);
        break;
    default:
        Serial.println(P("Can't use security"));
        return -1;
    }

    if (did_timeout_) {
        return -1;
    }

    if (!dhcp) {
        sprintf(cmd, P("AT+DNSSET=%d.%d.%d.%d"),
            _gateway[0], _gateway[1], _gateway[2], _gateway[3]);
        command(cmd, GSCOMMANDMODE_NORMAL);
    }

    _joined = true;
    _dhcp   = dhcp;
    return 0;
}

int GSwifi::listen(uint16_t port) {
    char cmd[GS_CMD_SIZE];

    if ( (! _joined) ||
         (_power_status != GSPOWERSTATUS_READY) ) {
        return -1;
    }

    sprintf(cmd, P("AT+NSTCP=%d"), port);
    command(cmd, GSCOMMANDMODE_CONNECT);
    if (did_timeout_) {
        return -1;
    }

    _listening   = true;
    serverRequest.cid = CID_UNDEFINED;

    // assume CID is 0 for server (only listen on 1 port)

    return 0;
}

int GSwifi::disconnect () {
    int i;

    _joined    = false;
    _listening = false;
    command(PB("AT+NCLOSEALL",1), GSCOMMANDMODE_NORMAL);
    command(PB("AT+WD",1),        GSCOMMANDMODE_NORMAL);
    command(PB("AT+NDHCP=0",1),   GSCOMMANDMODE_NORMAL);
    return 0;
}

int GSwifi::setAddress (char *name) {
    command(PB("AT+NDHCP=1",1), GSCOMMANDMODE_DHCP, GS_TIMEOUT2);
    if (did_timeout_) {
        return -1;
    }
    if (_ipaddr.isNull()) return -1;
    return 0;
}

int GSwifi::setAddress (IpAddr ipaddr, IpAddr netmask, IpAddr gateway, IpAddr nameserver) {
    int r;
    char cmd[GS_CMD_SIZE];

    command(PB("AT+NDHCP=0",1), GSCOMMANDMODE_NORMAL);
    // wait_ms(100);

    sprintf(cmd, P("AT+NSET=%d.%d.%d.%d,%d.%d.%d.%d,%d.%d.%d.%d"),
        ipaddr[0], ipaddr[1], ipaddr[2], ipaddr[3],
        netmask[0], netmask[1], netmask[2], netmask[3],
        gateway[0], gateway[1], gateway[2], gateway[3]);
    command(cmd, GSCOMMANDMODE_NORMAL);
    if (did_timeout_) {
        return -1;
    }
    _ipaddr = ipaddr;
    _netmask = netmask;
    _gateway = gateway;

    if (ipaddr != nameserver) {
        sprintf(cmd, P("AT+DNSSET=%d.%d.%d.%d"),
            nameserver[0], nameserver[1], nameserver[2], nameserver[3]);
        command(cmd, GSCOMMANDMODE_NORMAL);
    }
    return did_timeout_;
}

int GSwifi::getHostByName (const char* name, IpAddr &addr) {
    char cmd[GS_CMD_SIZE];

    if (! _joined || _power_status != GSPOWERSTATUS_READY) return -1;

    sprintf(cmd, P("AT+DNSLOOKUP=%s"), name);
    command(cmd, GSCOMMANDMODE_DNSLOOKUP);
    if (did_timeout_) {
        return -1;
    }

    addr = _resolv;
    return 0;
}

int GSwifi::getHostByName (Host &host) {
    char cmd[GS_CMD_SIZE];

    if (! _joined || _power_status != GSPOWERSTATUS_READY) return -1;

    sprintf(cmd, P("AT+DNSLOOKUP=%s"), host.getName());
    command(cmd, GSCOMMANDMODE_DNSLOOKUP);
    if (did_timeout_) {
        return -1;
    }

    host.setIp(_resolv);
    return 0;
}

bool GSwifi::isJoined () {
    return _joined;
}

bool GSwifi::isListening () {
    return _listening;
}

GSwifi::GSPOWERSTATUS GSwifi::getPowerStatus () {
    return _power_status;
}

// 4.2.1 UART Parameters
// Allowed baud rates include: 9600, 19200, 38400, 57600, 115200, 230400,460800 and 921600.
// The new UART parameters take effect immediately. However, they are stored in RAM and will be lost when power is lost unless they are saved to a profile using AT&W (section 4.6.1). The profile used in that command must also be set as the power-on profile using AT&Y (section 4.6.3).
// This command returns the standard command response (section 4) to the serial interface with the new UART configuration.
int8_t GSwifi::setBaud (uint32_t baud) {
    char cmd[GS_CMD_SIZE];

    if (_power_status != GSPOWERSTATUS_READY) {
        return -1;
    }

    sprintf(cmd, P("ATB=%ld"), baud);
    _serial->println(cmd);
    Serial.print(P("c> ")); Serial.println(cmd);

    delay(1000);

    _serial->end();
    _serial->begin(baud);

    delay(1000);

    // Skip 1st "ERROR: INVALID INPUT" after baud rate change
    command("", GSCOMMANDMODE_NORMAL);

    return 0;
}

int8_t GSwifi::setRegion (int reg) {
    char cmd[GS_CMD_SIZE];

    if (_power_status != GSPOWERSTATUS_READY) return -1;

    sprintf(cmd, P("AT+WREGDOMAIN=%d"), reg);
    command(cmd, GSCOMMANDMODE_NORMAL);
    return did_timeout_;
}

int8_t GSwifi::postDoor (const char *key, GSEventHandler handler) {
    _responseHandler = handler;

    char cmd[GS_CMD_SIZE]; // uses 62

    command(PB("AT+HTTPCONF=7,application/x-www-form-urlencoded",1), GSCOMMANDMODE_NORMAL);
    // Content-Length is fixed to 40
    command(PB("AT+HTTPCONF=5,40",1), GSCOMMANDMODE_NORMAL);

    sprintf(cmd, P("AT+HTTPOPEN=%s,80"), DOMAIN);
    command(cmd, GSCOMMANDMODE_HTTP);
    if (did_timeout_) {
        return -1;
    }

    // example POST body
    // key=23B31A70-4E14-4970-8737-BD478BC968E2
    // length: 40
    sprintf(cmd, P("AT+HTTPSEND=%d,3,%d,/door,40"),
            clientRequest.cid,
            GS_LONGPOLL_TIMEOUT);
    command(cmd, GSCOMMANDMODE_NORMAL, GS_LONGPOLL_TIMEOUT_MS);
    if (did_timeout_) {
        return -1;
    }

    sprintf(cmd, P("H%dkey=%s"), clientRequest.cid, key);
    escape( cmd, GS_IGNORE_TIMEOUT );

    // we're long polling here, to receive other events, we're going back to our main loop
    // ignore timeout, we always timeout here

    return 0;
}

int8_t GSwifi::getMessages (const char *key, GSEventHandler handler) {
    _responseHandler = handler;

    char cmd[87];

    command( PB("AT+HTTPCONFDEL=5",1), GSCOMMANDMODE_NORMAL);
    command( PB("AT+HTTPCONFDEL=7",1), GSCOMMANDMODE_NORMAL);

    sprintf(cmd, P("AT+HTTPOPEN=%s,80"), DOMAIN);
    command(cmd, GSCOMMANDMODE_HTTP);
    if (did_timeout_) {
        return -1;
    }

    // TODO newer_than
    // ex: AT+HTTPSEND=1,1,40,/messages?key=23B31A70-4E14-4970-8737-BD478BC968E2&newer_than=65535
    sprintf(cmd, P("AT+HTTPSEND=%d,1,%d,/messages?key=%s&newer_than=%d"),
            clientRequest.cid,
            GS_LONGPOLL_TIMEOUT,
            key,
            newest_message_id);
    command(cmd, GSCOMMANDMODE_NORMAL);

    // we're long polling here, to receive other events, we're going back to our main loop
    // ignore timeout, we always timeout here

    return 0;
}

#ifdef GS_ENABLE_MDNS
/**
 * mDNS
 */
int8_t GSwifi::mDNSStart() {
    command(PB("AT+MDNSSTART",1), GSCOMMANDMODE_NORMAL);
    if (did_timeout_) {
        return -1;
    }
    return 0;
}

int8_t GSwifi::mDNSRegisterHostname(const char *hostname) {
    char cmd[GS_CMD_SIZE];
    sprintf(cmd, P("AT+MDNSHNREG=%s,local"), hostname);
    command(cmd, GSCOMMANDMODE_NORMAL);
    if (did_timeout_) {
        return -1;
    }
    return 0;
}

int8_t GSwifi::mDNSDeregisterHostname(const char *hostname) {
    char cmd[GS_CMD_SIZE];
    sprintf(cmd, P("AT+MDNSHNDEREG=%s,local"), hostname);
    command(cmd, GSCOMMANDMODE_NORMAL);
    if (did_timeout_) {
        return -1;
    }
    return 0;
}

// AT+MDNSSRVREG=<ServiceInstanceName>,[<ServiceSubType>],<ServiceType>, <Protocol>,<Domain>,<port>,<Default Key=Val>,<key 1=val 1>, <key 2=val 2>.....
// Example: if the factory default host name is “GAINSPAN” and the mac address of the node is “00-1d-c9- 00-22-97”, then AT+MDNSHNREG=,local
// Will take the host name as “GAINSPAN_002297”
// TODO change factory default host name
int8_t GSwifi::mDNSRegisterService(const char *name, const char *subtype, const char *type, const char *protocol, uint16_t port) {
    char cmd[GS_CMD_SIZE];
    sprintf(cmd, P("AT+MDNSSRVREG=%s,%s,%s,%s,local,%d"), name, subtype, type, protocol, port );
    command(cmd, GSCOMMANDMODE_NORMAL);
    if (did_timeout_) {
        return -1;
    }
    return 0;
}

int8_t GSwifi::mDNSDeregisterService(const char *name, const char *subtype, const char *type, const char *protocol) {
    char cmd[GS_CMD_SIZE];
    sprintf(cmd, P("AT+MDNSSRVDEREG=%s,%s,%s,%s,local"), name, subtype, type, protocol );
    command(cmd, GSCOMMANDMODE_NORMAL);
    if (did_timeout_) {
        return -1;
    }
    return 0;
}

int8_t GSwifi::mDNSAnnounceService() {
    command(PB("AT+MDNSANNOUNCE",1), GSCOMMANDMODE_NORMAL);
    if (did_timeout_) {
        return -1;
    }
    return 0;
}

int8_t GSwifi::mDNSDiscoverService(const char *subtype, const char *type, const char *protocol) {
    char cmd[GS_CMD_SIZE];
    sprintf(cmd, P("AT+MDNSSD=%s,%s,%s,local"), subtype, type, protocol);
    command(cmd, GSCOMMANDMODE_NORMAL);
    if (did_timeout_) {
        return -1;
    }
    return 0;
}
#endif // GS_ENABLE_MDNS

// for test
void GSwifi::dump () {
    Serial.print(P("_joined:"));            Serial.println(_joined);
    Serial.print(P("_power_status:"));      Serial.println(_power_status);
    Serial.print(P("did_timeout_:"));       Serial.println(did_timeout_);
    Serial.print(P("_gs_response_lines:")); Serial.println(_gs_response_lines);
    Serial.print(P("_gs_mode:"));           Serial.println(_gs_mode);
}