/*

	Fully workable FLTK interactive terminal using
	an improved 'Fl_Terminal' widget.

	(c) 2026 wcout@gmx.net

	It is running many applications like editors (vim) and
	tools like w3m or btop nearly perfectly.

	Initially written for Linux only, but later tried to be made
	cross platform using Google Gemini.

	Under Windows (tested only on WIN11), it uses a reader thread
   to fetch the output to the terminal without blocking UI.
   On Linux/macOs it uses the Fl::add_fd() callback mechanism.

*/
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Terminal.H>
#include <FL/fl_draw.H>
// OS-specific includes & globals
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#endif
#ifdef _WIN32
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00

#include <windows.h>
#include <wincon.h>
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <errno.h>
#endif

// PTY terminal class with resize logic
class Fl_PTY_Terminal : public Fl_Terminal {
public:
  Fl_PTY_Terminal(int x_, int y_, int w_, int h_, const char* l_ = nullptr);
  ~Fl_PTY_Terminal();
#ifndef _WIN32
private:
  void onRead(int fd_, void *data_);
  static void pty_read_callback(int fd_, void *data_);
#else
  static DWORD WINAPI ChildReaderThread(LPVOID lpParam);
  static VOID CALLBACK WaitCallback(PVOID lpParameter, BOOLEAN TimerOrWaitFired);
#endif
  int handle_dnd(int event);
  int handle_paste(const std::string &text);
protected:
  void write_pty(const char *buf_, size_t len_);
  bool init();
  void quit();
public:
  void dump(const char *buf, size_t n);
  void input(const std::string& buffer);
  void send_pty(const char *buf_) override;
  int handle(int event) override;
  void resize(int X, int Y, int W, int H) override;
  void char_to_pixel_dimension(int columns_, int rows_, int &w_, int &h_);
  void command(const char *cmd_);
  void logging(int level_, FILE *logfile_ = stderr) {
    _log = level_;
    _logfile = logfile_;
  }
  bool inited() const {
    return _inited;
  }
private:
  int  _pty_master_fd;
  bool _inited;
  int _log;
  FILE *_logfile;
#ifdef _WIN32
  HANDLE _hPipeInWrite = INVALID_HANDLE_VALUE;
  HANDLE _hPipeOutRead = INVALID_HANDLE_VALUE;
  HPCON _hPC = nullptr;
  HANDLE _hWaitHandle = NULL;
public:
  HANDLE readPipe() const	{
    return _hPipeOutRead;
  }
#endif
};

////////////////////////////////////////////////////////////////////////////////////////////////
//  PTY terminal class with resize logic implementation
////////////////////////////////////////////////////////////////////////////////////////////////
Fl_PTY_Terminal::Fl_PTY_Terminal(int x_, int y_, int w_, int h_, const char* l_/* = nullptr*/) :
  Fl_Terminal(x_, y_, w_, h_, l_),
  _pty_master_fd(-1),
  _inited(false),
  _log(0),
  _logfile(stderr)
#ifdef _WIN32
  ,_hPipeInWrite(INVALID_HANDLE_VALUE),
  _hPipeOutRead(INVALID_HANDLE_VALUE),
  _hPC(nullptr)
#endif
{
  _inited = init();
  if (_inited) {
#ifndef _WIN32
    Fl::add_fd(_pty_master_fd, FL_READ, pty_read_callback, this);
#endif
  }
}

Fl_PTY_Terminal::~Fl_PTY_Terminal() {
#ifdef _WIN32
  if (_hPC) {
    ClosePseudoConsole(_hPC);
  }
  if (_hPipeInWrite != INVALID_HANDLE_VALUE) {
    CloseHandle(_hPipeInWrite);
  }
#else
  if (_pty_master_fd >= 0) {
    ::close(_pty_master_fd);
  }
#endif
}

void Fl_PTY_Terminal::dump(const char *buf, size_t n) {
  static const char BOLD[] = "\033[1m";
  static const char TEXT[] = "\033[90m";
  static const char HEX[] = "\033[31m";
  static const char CTRL[] = "\033[1m\033[32m";
  static const char OFF[] = "\033[0m";

  bool space(false);
  for (size_t i = 0; i < n; i++) {
    unsigned int c = (unsigned char)buf[i];
    if (c == 27) {
      if (space) {
        fprintf(_logfile, " ");
      }
      fprintf(_logfile, "%s\\e%s", BOLD, OFF);
      space = false;
    } else if (c >= ' ' && c <= 127) {
      if (space) {
        fprintf(_logfile, " ");
      }
      fprintf(_logfile, "%s%c%s", TEXT, c, OFF);
      space = false;
    } else {
      if (space) {
        fprintf(_logfile, " ");
      }
      switch (c) {
        case '\r':
          fprintf(_logfile, "%s\\r%s", CTRL, OFF);
          break;
        case '\n':
          fprintf(_logfile, "%s\\n%s", CTRL, OFF);
          break;
        case '\a':
          fprintf(_logfile, "%s\\a%s", CTRL, OFF);
          break;
        case '\t':
          fprintf(_logfile, "%s\\t%s", CTRL, OFF);
          break;
        default:
          fprintf(_logfile, "%s\\x%02X%s", HEX, (unsigned char)c, OFF);
      }
//			space = true; // NOTE: not needed any more
    }
  }
  fprintf(_logfile, "\n");
}

void Fl_PTY_Terminal::write_pty(const char *buf_, size_t len_) {
  if (_pty_master_fd != -1) {
#ifdef _WIN32
    DWORD bytes_written = 0;
    if (_hPipeInWrite != INVALID_HANDLE_VALUE) {
      WriteFile(_hPipeInWrite, buf_, (DWORD)len_, &bytes_written, nullptr);
    }
#else
    if (_pty_master_fd >= 0) {
      ::write(_pty_master_fd, buf_, len_);
    }
#endif
  }
}

#ifdef _WIN32
/*static*/
VOID CALLBACK Fl_PTY_Terminal::WaitCallback(PVOID lpParameter, BOOLEAN TimerOrWaitFired) {
  Fl_PTY_Terminal *term = (Fl_PTY_Terminal *)lpParameter;
  term->input("");
}
#endif

bool Fl_PTY_Terminal::init() {
  // Initialize backend
#ifdef _WIN32
  auto enable_VT_mode =[&](void) -> bool {
    // Enable VT mode (Color)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) {
      return false;
    }

    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) {
      return false;
    }

    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
    return true;
  };

  enable_VT_mode();

  // WINDOWS ConPTY
  HANDLE hPipeInRead = INVALID_HANDLE_VALUE;
  HANDLE hPipeOutWrite = INVALID_HANDLE_VALUE;
  SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };

  if (!CreatePipe(&hPipeInRead, &_hPipeInWrite, &sa, 0) ||
      !CreatePipe(&_hPipeOutRead, &hPipeOutWrite, &sa, 0)) {
    return false;
  }

  SetHandleInformation(_hPipeInWrite, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(_hPipeOutRead, HANDLE_FLAG_INHERIT, 0);

  COORD size = { 80, 25 };
  HRESULT hr = CreatePseudoConsole(size, hPipeInRead, hPipeOutWrite, 0, &_hPC);
  CloseHandle(hPipeInRead);
  CloseHandle(hPipeOutWrite);

  if (FAILED(hr)) {
    return false;
  }

  STARTUPINFOEXW siEx = { 0 };
  siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);
  SIZE_T sizeList = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &sizeList);
  siEx.lpAttributeList = (PPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, sizeList);
  InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &sizeList);

  UpdateProcThreadAttribute(siEx.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, _hPC, sizeof(HPCON), nullptr, nullptr);
  siEx.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
  siEx.StartupInfo.wShowWindow = SW_HIDE;

  PROCESS_INFORMATION pi = { 0 };
  wchar_t cmdPath[] = L"C:\\Windows\\System32\\cmd.exe";

  // For Windows PowerShell
//	wchar_t cmdPath[] = L"powershell.exe -NoLogo -NoExit -Command chcp 65001";

  // Or for PowerShell 7+
//		wchar_t cmdPath[] = L"pwsh.exe -NoLogo -NoExit";

  BOOL success = CreateProcessW(nullptr, cmdPath, nullptr, nullptr, TRUE,
                                EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                &siEx.StartupInfo, &pi);
  if (!success) {
    return false;
  }

  RegisterWaitForSingleObject(&_hWaitHandle, pi.hProcess, WaitCallback, this, INFINITE, WT_EXECUTEONLYONCE);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  _pty_master_fd = _open_osfhandle((intptr_t)_hPipeOutRead, _O_RDONLY | _O_BINARY);

  // Spawn background reader thread
  CreateThread(NULL, 0, ChildReaderThread, this, 0, NULL);

  return true;
#else
  // Use POSIX PTY (macOS/Linux)
  _pty_master_fd = ::posix_openpt(O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (_pty_master_fd < 0) {
    return false;
  }

  if (::grantpt(_pty_master_fd) < 0 || ::unlockpt(_pty_master_fd) < 0) {
    ::close(_pty_master_fd);
    return false;
  }

  char* slave_name = ::ptsname(_pty_master_fd);
  if (!slave_name) {
    ::close(_pty_master_fd);
    return false;
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(_pty_master_fd);
    return false;
  }

  if (pid == 0) {
    ::setsid();
    int slave_fd = ::open(slave_name, O_RDWR);
    if (slave_fd < 0) {
      std::exit(1);
    }
    ::close(_pty_master_fd);

#ifdef TIOCSCTTY
    ::ioctl(slave_fd, TIOCSCTTY, 0);
#endif

    ::dup2(slave_fd, STDIN_FILENO);
    ::dup2(slave_fd, STDOUT_FILENO);
    ::dup2(slave_fd, STDERR_FILENO);
    ::close(slave_fd);

#ifdef __APPLE__
    ::execl("/bin/zsh", "zsh", "--login", nullptr);
#else
    ::execl("/bin/bash", "bash", "--login", nullptr);
#endif
    std::exit(1);
  }
  return true;
#endif
}

void Fl_PTY_Terminal::quit() {
  append("\n--- shell process stopped ---\n");
//	close(_pty_master_fd);
  window()->hide(); // shut down terminal
}

void Fl_PTY_Terminal::input(const std::string& buffer) {
  if (buffer.empty()) {
    quit();
    return;
  }
  size_t bytes_read = buffer.size();
  const char *buf = buffer.c_str();
  if (bytes_read > 0) {
    if (_log > 1) {
      fprintf(_logfile, "Send %d bytes to terminal:\n", (int)bytes_read);
      dump(buf, bytes_read);
    }
    append(buf);
  }
}

#ifdef _WIN32
// Background thread loop for reading stdout
/*static*/
DWORD WINAPI Fl_PTY_Terminal::ChildReaderThread(LPVOID lpParam) {
  // Structure to pass data from background thread to FLTK main thread
  struct MessageData {
    Fl_PTY_Terminal *term;
    std::string text;
  };
  Fl_PTY_Terminal *term = reinterpret_cast<Fl_PTY_Terminal *>(lpParam);
  HANDLE hReadPipe = term->readPipe();
  char buffer[4096];
  DWORD bytesRead = 0;

  // Blocks efficiently in kernel mode when no data is available
  while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
    buffer[bytesRead] = '\0';

    MessageData* msg = new MessageData{ term, std::string(buffer, bytesRead) };

    // Notify FLTK main thread to run fltk_update_callback
    Fl::awake([](void *data) {
      MessageData* msg = reinterpret_cast<MessageData*>(data);
      msg->term->input(msg->text);
      delete msg;
    }, msg);
  }

  // Clean up handle and notify GUI of termination
  CloseHandle(hReadPipe);
  MessageData* msg = new MessageData{ term, "" };
  Fl::awake([](void *data) {
    MessageData* msg = reinterpret_cast<MessageData*>(data);
    msg->term->input(msg->text);
    delete msg;
  }, msg);

  return 0;
}
#else
void Fl_PTY_Terminal::onRead(int fd_, void *data_) {
  char buffer[4096];
  bool process_alive = true;
  int bytes_read = 0;

  // Under macOS/Linux we use POSIX read()
  ssize_t posix_bytes = ::read(fd_, buffer, sizeof(buffer) - 1);
  if (posix_bytes > 0) {
    bytes_read = (int)posix_bytes;
  } else if (posix_bytes == 0 || (posix_bytes < 0 && errno != EAGAIN)) {
    process_alive = false;
  }

  if (bytes_read > 0) {
    buffer[bytes_read] = '\0';
    input(std::string(buffer));
  }

  if (!process_alive) {
    Fl::remove_fd(fd_);
    input("");
  }
}

// Linux pty fd callback
/*static*/
void Fl_PTY_Terminal::pty_read_callback(int fd_, void *data_) {
  Fl_PTY_Terminal* terminal = static_cast<Fl_PTY_Terminal*>(data_);
  terminal->onRead(fd_, data_);
}
#endif

void Fl_PTY_Terminal::send_pty(const char *buf_) {
  int len = strlen(buf_);
  if (_log) {
    fprintf(_logfile, "Terminal answer (%d bytes): ", len);
    dump(buf_, len);
  }
  write_pty(buf_, len);
}

int Fl_PTY_Terminal::handle_paste(const std::string &text) {
  if (text.size() && !alternate_buffer()) {
    if (bracketed_paste()) {
      write_pty("\x1b[200~", 6);
    }
    write_pty(text.c_str(), text.size());
    if (bracketed_paste()) {
      write_pty("\x1b[201~", 6);
    }
    return 1;
  }
  return 0;
}

int Fl_PTY_Terminal::handle_dnd(int event) {
  switch (event) {
    case FL_DND_ENTER:
    case FL_DND_DRAG:
    case FL_DND_RELEASE:
      return Fl::event_inside(this);
    case FL_PASTE:
      return handle_paste(std::string(Fl::event_text(), Fl::event_length()));
  }
  return 0;
}

// Catch Keyboard strokes
int Fl_PTY_Terminal::handle(int event) {
  if (event == FL_KEYDOWN && _pty_master_fd != -1) {
    int key = Fl::event_key();
    int state = Fl::event_state();

    // Catch TAB key for auto completion
    if (key == FL_Tab) {
      char tab_char = '\t';
      write_pty(&tab_char, 1);
      return 1; // 1 signals to FLTK: event processed (prevents focus change)
    }

    // Catch Strg (Ctrl) key combinations (e.g. Strg+C)
    if (state & FL_CTRL) {
      // Keep FLTK zoom keys functional
      if (key == '+' || key == '-' || key == '0') {
        return Fl_Terminal::handle(event);
      }
      if ((key == 'c' || key == 'C') && Fl_Terminal::is_selection()) {
        return Fl_Terminal::handle(event);
      } else if ((key == 'a' || key == 'A') && !alternate_buffer()) {
        return Fl_Terminal::handle(event);
      } else if ((key == 'v' || key == 'V') && !alternate_buffer()) {
        if (Fl::clipboard_contains(Fl::clipboard_plain_text)) {
          Fl::paste(*this, 1, Fl::clipboard_plain_text);
          return 1;
        }
      }

      // In ASCII Strg+A = 1, Strg+B = 2, Strg+C = 3, ...
      if (key >= 'a' && key <= 'z') {
        char ctrl_char = key - 'a' + 1;
        write_pty(&ctrl_char, 1);
        return 1;
      }
      // Catch uppercase letters, if Capslock/Shift is active
      else if (key >= 'A' && key <= 'Z') {
        char ctrl_char = key - 'A' + 1;
        write_pty(&ctrl_char, 1);
        return 1;
      }
    }

    // Arrow keys for command history (up/down) and Cursor navigation
    if (key == FL_Up) {
      write_pty((DECCKM() ? "\033OA" : "\033[A"), 3); // ANSI Escape-Code for Arror up
      return 1;
    }
    if (key == FL_Down) {
      write_pty((DECCKM() ? "\033OB" : "\033[B"), 3); // ANSI Escape-Code for Arrow down
      return 1;
    }
    if (key == FL_Right) {
      write_pty((DECCKM() ? "\033OC" :"\033[C"), 3); // ANSI Escape-Code for Arrow right
      return 1;
    }
    if (key == FL_Left) {
      write_pty((DECCKM() ? "\033OD" : "\033[D"), 3); // ANSI Escape-Code for Arrow left
      return 1;
    }
    if (key == FL_Delete) {
      write_pty("\033[3~", 4); // ANSI Escape-Code for Delete key
      return 1;
    }
    if (key == FL_Home) {
      if (state & FL_CTRL) {
        write_pty("\033[1;5H", 6);  // ANSI Escape-Code for Home key
      } else {
        write_pty("\033[H", 3);  // ANSI Escape-Code for Home key
      }
      return 1;
    }
    if (key == FL_End) {
//				write_pty("\033[4~", 4); // ANSI Escape-Code for End key
      if (state & FL_CTRL) {
        write_pty("\033[1;5F", 6); // ANSI Escape-Code for End key
      } else {
//					write_pty("\033[4~", 4); // ANSI Escape-Code for End key
        write_pty("\033[F", 3); // ANSI Escape-Code for End key
      }
      return 1;
    }
    if (key >= FL_F + 1 && key <= FL_F + 5) {
      char buf[7];
      snprintf(buf, sizeof(buf), "\033[%d~", key - FL_F - 1 + 11);
      write_pty(buf, 5); // ANSI Escape-Code for F1-F5
      return 1;
    }
    if (key >= FL_F + 6 && key <= FL_F + 10) {
      char buf[7];
      snprintf(buf, sizeof(buf), "\033[%d~", key - FL_F - 6 + 17);
      write_pty(buf, 5); // ANSI Escape-Code for F1-F10
      return 1;
    }
    if (key == FL_F + 11) {
      // toggle logging
      _log++;
      _log %= 3;
      fprintf(_logfile, "logging: %d\n", _log);
      return 1;
    }
    if (key == FL_F + 12) {
      // toggle dark/light mode
      if (color() == FL_WHITE) {
        color(FL_BLACK);
      } else {
        color(FL_WHITE);
      }
      return 1;
    }
    if (key == FL_Page_Up) {
      write_pty("\033[5~", 4); // ANSI Escape-Code for Page up
      return 1;
    }
    if (key == FL_Page_Down) {
      write_pty("\033[6~", 4); // ANSI Escape-Code for Page down
      return 1;
    }

    // Handle ALT keys
    if (key >='a' && key <= 'z' && (state & FL_ALT)) {
      char buf[4];
      snprintf(buf, sizeof(buf), "\033%c", key);
      write_pty(buf, 2);
      return 1;
    }

    // Standard textinput (normal chars and Enter)
    const char* text = Fl::event_text();
    int len = Fl::event_length();
    if (len > 0) {
      write_pty(text, len);
      return 1;
    }
  }
  if (handle_dnd(event)) {
    return 1;
  }

  return Fl_Terminal::handle(event);
}

// Catch GUI-Resize and pass it on to the Bash
void Fl_PTY_Terminal::resize(int X, int Y, int W, int H) {
  // First let the base class change the GUI-Widget size
  Fl_Terminal::resize(X, Y, W, H);

  // Only change, when PTY is already active
  if (_pty_master_fd != -1) {
    // Calculate columns and rows for current size
    int columns = display_columns();
    int rows    = display_rows();
    if (_log) {
      fprintf(_logfile, "resized to: %d x %d\n", columns, rows);
    }

    // Prevent invalid values by extreme minimize
    if (columns < 10) {
      columns = 10;
    }
    if (rows < 2) {
      rows = 2;
    }

#ifdef _WIN32
    if (_hPC != nullptr) {
      COORD size = { (short)columns, (short)rows };
      // inform Windows ConPTY
      ResizePseudoConsole(_hPC, size);
    }
#else
    if (_pty_master_fd >= 0) {
      // Fill OS structure for terminal size
      struct winsize ws;
      ws.ws_col = static_cast<unsigned short>(columns);
      ws.ws_row = static_cast<unsigned short>(rows);
      ws.ws_xpixel = static_cast<unsigned short>(W);
      ws.ws_ypixel = static_cast<unsigned short>(H);

      // Signal to pseudoterminal (and shell) the new size
      ioctl(_pty_master_fd, TIOCSWINSZ, &ws);
    }
#endif
  }
}

void Fl_PTY_Terminal::char_to_pixel_dimension(int columns_, int rows_, int &w_, int &h_) {
  fl_font(textfont(), textsize());
  int pxh = fl_height() * rows_;
  int pxw = fl_width('X') * columns_;
  int dw = Fl::box_dw(box()) + scrollbar->w();
  int dh = Fl::box_dh(box());

  w_ = pxw + dw + margin_left() + margin_right();
  h_ = pxh + dh + margin_top() + margin_bottom();
}

void Fl_PTY_Terminal::command(const char *cmd_) {
  write_pty(cmd_, strlen(cmd_));
  write_pty("\n", 1);
}

////////////////////////////////////////////////////////////////////////////////////////////////

static int log_ = 0;
static int no_splash = 0;
static FILE *logfile = nullptr;
static int set_term = 0;
static int history_lines = -1; // -1: Fl_Terminal default (100)
static int columns = 80;
static int rows = 25;
static int color = FL_WHITE;
static std::string cmd; // initial command (none)

void parse_command_line(int argc, char *argv[]) {
  auto geometry = [&](const std::string &s_, int &w_, int &h_) ->void {
    size_t pos = s_.find('x');
    if (pos == std::string::npos) {
      return;
    }
    int w = atoi(s_.substr(0, pos).c_str());
    int h = atoi(s_.substr(pos + 1).c_str());
    if (w > 10 && w <= 132 && h > 10 && h <= 50) {
      w_ = w;
      h_ = h;
    }
  };
  auto test_arg =[&](const char *arg, char v, int &var) -> bool {
    size_t n = 0;
    if (arg[0] == '-') {
      while (arg[n + 1] == v) {
        var++;
        n++;
      }
    }
    return n;
  };

  for (int i = 1; i < argc; i++) {
    if (argv[i][0] != '-') {
      cmd = argv[i];
    } else {
      if (std::string(argv[i]) == "--help") {
        fprintf(stderr, "Usage:\n"
                "-D ....... dark mode\n"
                "-l[l] .... log input [and output]\n"
                "-F name .. log to file\n"
                "-g WxH ... geometry width x height chars\n"
                "-h n ..... use n history lines\n"
                "-s ....... don't show splash screen\n"
                "-t ....... set TERM=xterm-256color\n");
        exit(0);
      }
      if (argv[i][1] == 'D') {
        color = 0x10101000; // dark mode
      }
      test_arg(argv[i], 'l', log_);
      test_arg(argv[i], 't', set_term);
      test_arg(argv[i], 's', no_splash);
      if (argv[i][1] == 'h') {
        if (i + 1 < argc) {
          i++;
          if (argv[i][0] >= '0' && argv[i][0] <= '9') {
            history_lines = atoi(argv[i]);
          }
        }
      }
      if (argv[i][1] == 'g') {
        if (i + 1 < argc) {
          i++;
          geometry(argv[i], columns, rows);
        }
      }
      if (argv[i][1] == 'F') {
        if (i + 1 < argc) {
          i++;
          logfile = fopen(argv[i], "w");
        }
      }
    }
  }
}

int main(int argc, char** argv) {
  parse_command_line(argc, argv);
#ifdef _WIN32
  Fl::lock();
#else
  if (set_term) {
    setenv("TERM", "xterm-256color", 1);
  }
#endif

  // we must create terminal on heap, otherwise FLTK's auto-deleter chokes...
  Fl_PTY_Terminal *terminal = new Fl_PTY_Terminal(0, 0, 100, 100); // create smaller than will be later
  Fl_PTY_Terminal &term = *terminal;
  if (!term.inited()) {
    fprintf(stderr, "Failed to initialize PTY backend\n");
    return 1;
  }
  term.logging(log_, logfile ? logfile : stderr);
  term.textsize(18);
  term.color(color);
  term.hscrollbar_style(Fl_Terminal::SCROLLBAR_OFF);
  term.scrollbar_size(6);
  term.scrollbar->color(FL_WHITE, FL_RED);

  // calculate how the big the window must be for the terminal
  int win_w, win_h;
  term.char_to_pixel_dimension(columns, rows, win_w, win_h);

  // create window in just the right size
  Fl_Double_Window win(win_w, win_h, "FLTK Terminal");
  win.add(term); // no add the terminal to it
  win.resizable(term);

  // Send inital terminal size to PTY after window was shown
  term.resize(0, 0, win.w(), win.h()); // resize to full window size
  win.end();
  win.show(/*argc, argv*/);

  ::printf("PTTY %d x %d\n", term.display_columns(), term.display_rows());

  term.callback([](Fl_Widget *wgt_, void *d_) {
    const char *buf = (const char *)d_;
    if (log_) {
      Fl_PTY_Terminal *term = (Fl_PTY_Terminal *)wgt_;
      int reason = Fl::callback_reason();
      fprintf(logfile ? logfile : stderr, "Fl_Terminal NOT IMPLEMENTED (%d): ", reason);
      term->dump(buf, strlen(buf));
    }
  });

  if (history_lines >= 0) {
    term.history_lines(history_lines);
  }
  if (cmd.empty() && !no_splash) {
    cmd =
#ifdef _WIN32
#include "fltk_term_splash_win32.h"
#else
#include "fltk_term_splash_linux.h"
#endif
    ;
  }
  if (cmd.size()) {
    Fl::add_timeout(0., [](void *d_) {
      Fl_PTY_Terminal *term = (Fl_PTY_Terminal *)d_;
      term->command(cmd.c_str());
    }, terminal);
  }

  int result = Fl::run();

  // Clean-up
  if (logfile) {
    fclose(logfile);
  }
  delete terminal;
  return result;
}
