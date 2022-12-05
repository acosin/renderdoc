/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2022 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "renderdoccmd.h"
#include <dlfcn.h>
#include <iconv.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>
#include "../renderdoc/head.h"

#if defined(RENDERDOC_WINDOWING_XLIB)
#include <X11/Xlib-xcb.h>
#endif

#include <replay/renderdoc_replay.h>

void Daemonise()
{
  // don't change dir, but close stdin/stdou
  daemon(1, 0);
}

static Display *display = NULL;

WindowingData DisplayRemoteServerPreview(bool active, const rdcarray<WindowingSystem> &systems)
{
  static WindowingData remoteServerPreview = {WindowingSystem::Unknown};

// we only have the preview implemented for platforms that have xlib & xcb. It's unlikely
// a meaningful platform exists with only one, and at the time of writing no other windowing
// systems are supported on linux for the replay
#if defined(RENDERDOC_WINDOWING_XLIB) && defined(RENDERDOC_WINDOWING_XCB)
  if(active)
  {
    if(remoteServerPreview.system == WindowingSystem::Unknown)
    {
      // if we're first initialising, create the window
      if(display == NULL)
        return remoteServerPreview;

      int scr = DefaultScreen(display);

      xcb_connection_t *connection = XGetXCBConnection(display);

      if(connection == NULL)
      {
        std::cerr << "Couldn't get XCB connection from Xlib Display" << std::endl;
        return remoteServerPreview;
      }

      XSetEventQueueOwner(display, XCBOwnsEventQueue);

      const xcb_setup_t *setup = xcb_get_setup(connection);
      xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
      while(scr-- > 0)
        xcb_screen_next(&iter);

      xcb_screen_t *screen = iter.data;

      uint32_t value_mask, value_list[32];

      xcb_window_t window = xcb_generate_id(connection);

      value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
      value_list[0] = screen->black_pixel;
      value_list[1] =
          XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;

      xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 1280, 720, 0,
                        XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, value_mask, value_list);

      /* Magic code that will send notification when window is destroyed */
      xcb_intern_atom_cookie_t cookie = xcb_intern_atom(connection, 1, 12, "WM_PROTOCOLS");
      xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection, cookie, 0);

      xcb_intern_atom_cookie_t cookie2 = xcb_intern_atom(connection, 0, 16, "WM_DELETE_WINDOW");
      xcb_intern_atom_reply_t *atom_wm_delete_window = xcb_intern_atom_reply(connection, cookie2, 0);

      xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME,
                          XCB_ATOM_STRING, 8, sizeof("Remote Server Preview") - 1,
                          "Remote Server Preview");

      xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, (*reply).atom, 4, 32, 1,
                          &(*atom_wm_delete_window).atom);
      free(reply);

      xcb_map_window(connection, window);

      bool xcb = false, xlib = false;

      for(size_t i = 0; i < systems.size(); i++)
      {
        if(systems[i] == WindowingSystem::Xlib)
          xlib = true;
        if(systems[i] == WindowingSystem::XCB)
          xcb = true;
      }

      // prefer xcb
      if(xcb)
        remoteServerPreview = CreateXCBWindowingData(connection, window);
      else if(xlib)
        remoteServerPreview = CreateXlibWindowingData(display, (Drawable)window);

      xcb_flush(connection);
    }
    else
    {
      // otherwise, we can pump messages here, but we don't actually care to process any. Just clear
      // the queue
      xcb_generic_event_t *event = NULL;

      xcb_connection_t *connection = remoteServerPreview.xcb.connection;

      if(remoteServerPreview.system == WindowingSystem::Xlib)
        connection = XGetXCBConnection(remoteServerPreview.xlib.display);

      if(connection)
      {
        do
        {
          event = xcb_poll_for_event(connection);
          if(event)
            free(event);
        } while(event);
      }
    }
  }
  else
  {
    // reset the windowing data to 'no window'
    remoteServerPreview = {WindowingSystem::Unknown};
  }
#endif

  return remoteServerPreview;
}

void DisplayRendererPreview(IReplayController *renderer, TextureDisplay &displayCfg, uint32_t width,
                            uint32_t height, uint32_t numLoops)
{
// we only have the preview implemented for platforms that have xlib & xcb. It's unlikely
// a meaningful platform exists with only one, and at the time of writing no other windowing
// systems are supported on linux for the replay
#if defined(RENDERDOC_WINDOWING_XLIB) && defined(RENDERDOC_WINDOWING_XCB)
  // need to create a hybrid setup xlib and xcb in case only one or the other is supported.
  // We'll prefer xcb

  if(display == NULL)
  {
    std::cerr << "Couldn't open X Display" << std::endl;
    return;
  }

  int scr = DefaultScreen(display);

  xcb_connection_t *connection = XGetXCBConnection(display);

  if(connection == NULL)
  {
    std::cerr << "Couldn't get XCB connection from Xlib Display" << std::endl;
    return;
  }

  XSetEventQueueOwner(display, XCBOwnsEventQueue);

  const xcb_setup_t *setup = xcb_get_setup(connection);
  xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
  while(scr-- > 0)
    xcb_screen_next(&iter);

  xcb_screen_t *screen = iter.data;

  uint32_t value_mask, value_list[32];

  xcb_window_t window = xcb_generate_id(connection);

  value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  value_list[0] = screen->black_pixel;
  value_list[1] =
      XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;

  xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, width, height, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, value_mask, value_list);

  /* Magic code that will send notification when window is destroyed */
  xcb_intern_atom_cookie_t cookie = xcb_intern_atom(connection, 1, 12, "WM_PROTOCOLS");
  xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection, cookie, 0);

  xcb_intern_atom_cookie_t cookie2 = xcb_intern_atom(connection, 0, 16, "WM_DELETE_WINDOW");
  xcb_intern_atom_reply_t *atom_wm_delete_window = xcb_intern_atom_reply(connection, cookie2, 0);

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING,
                      8, sizeof("renderdoccmd") - 1, "renderdoccmd");

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, (*reply).atom, 4, 32, 1,
                      &(*atom_wm_delete_window).atom);
  free(reply);

  xcb_map_window(connection, window);

  rdcarray<WindowingSystem> systems = renderer->GetSupportedWindowSystems();

  bool xcb = false, xlib = false;

  for(size_t i = 0; i < systems.size(); i++)
  {
    if(systems[i] == WindowingSystem::Xlib)
      xlib = true;
    if(systems[i] == WindowingSystem::XCB)
      xcb = true;
  }

  IReplayOutput *out = NULL;

  // prefer xcb
  if(xcb)
  {
    out = renderer->CreateOutput(CreateXCBWindowingData(connection, window),
                                 ReplayOutputType::Texture);
  }
  else if(xlib)
  {
    out = renderer->CreateOutput(CreateXlibWindowingData(display, (Drawable)window),
                                 ReplayOutputType::Texture);
  }
  else
  {
    std::cerr << "Neither XCB nor XLib are supported, can't create window." << std::endl;
    std::cerr << "Supported systems: ";
    for(size_t i = 0; i < systems.size(); i++)
      std::cerr << (uint32_t)systems[i] << std::endl;
    std::cerr << std::endl;
    return;
  }

  out->SetTextureDisplay(displayCfg);

  xcb_flush(connection);

  uint32_t loopCount = 0;

  bool done = false;
  while(!done)
  {
    xcb_generic_event_t *event;

    event = xcb_poll_for_event(connection);
    if(event)
    {
      switch(event->response_type & 0x7f)
      {
        case XCB_EXPOSE: break;
        case XCB_CLIENT_MESSAGE:
          if((*(xcb_client_message_event_t *)event).data.data32[0] == (*atom_wm_delete_window).atom)
          {
            done = true;
          }
          break;
        case XCB_KEY_RELEASE:
        {
          const xcb_key_release_event_t *key = (const xcb_key_release_event_t *)event;

          if(key->detail == 0x9)
            done = true;
        }
        break;
        case XCB_DESTROY_NOTIFY: done = true; break;
        default: break;
      }
      free(event);
    }

    renderer->SetFrameEvent(10000000, true);
    out->Display();

    usleep(100000);

    loopCount++;

    if(numLoops > 0 && loopCount == numLoops)
      break;
  }
#else
  std::cerr << "No supporting windowing systems defined at build time (xlib and xcb)" << std::endl;
#endif
}

void sig_handler(int signo)
{
  if(usingKillSignal)
    killSignal = true;
  else
    exit(1);
}
#define USE_RENDER_DOC_CMD
int main(int argc, char *argv[], char* penv[])
{


#if 0
  const char *workdir = "/home/nvidia/workspace/wqg/QingLong/";
  chdir(workdir);
  
  const char *hmi = "hmi";
  char *argv_buf[4];
  argv_buf[0] = (char*)"hmi";
  argv_buf[1] = (char*)"config/hmi/hmi_config.json";
  argv_buf[2] = (char*)"--work-mode=1";
  argv_buf[3] = 0;
  char * env_buf[80];
  int idx = 0;
  env_buf[idx++] = (char*)"CLUTTER_IM_MODULE=xim";
  env_buf[idx++] = (char*)"CMAKE_PREFIX_PATH=/opt/ros/melodic";
  env_buf[idx++] = (char*)"COLORTERM=truecolor";
  env_buf[idx++] = (char*)"COLUMNS=204";
  env_buf[idx++] = (char*)"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus ";
  env_buf[idx++] = (char*)"DESKTOP_SESSION=unity";
  env_buf[idx++] = (char*)"DISPLAY=:0";
  env_buf[idx++] = (char*)"GDMSESSION=unity";
  env_buf[idx++] = (char*)"GNOME_DESKTOP_SESSION_ID=this-is-deprecated";
  env_buf[idx++] = (char*)"GNOME_TERMINAL_SCREEN=/org/gnome/Terminal/screen/366cdcf6_13b0_43e7_a641_ff9989aa218a";

  //10
    env_buf[idx++] = (char*)"GNOME_TERMINAL_SERVICE=:1.90";
  env_buf[idx++] = (char*)"GPG_AGENT_INFO=/run/user/1000/gnupg/S.gpg-agent:0:1";
  env_buf[idx++] = (char*)"GTK_CSD=0";
  env_buf[idx++] = (char*)"GTK_IM_MODULE=ibus";
  env_buf[idx++] = (char*)"GTK_MODULES=gail:atk-bridge";
  env_buf[idx++] = (char*)"HOME=/home/nvidia";
  env_buf[idx++] = (char*)"IM_CONFIG_PHASE=2";
  env_buf[idx++] = (char*)"INVOCATION_ID=219f44c38dce4ef7a643ea3e276b8273";
  env_buf[idx++] = (char*)"JOURNAL_STREAM=8:43582";
  env_buf[idx++] = (char*)"LANG=en_US.UTF-8";

//20
    env_buf[idx++] = (char*)"LC_ADDRESS=zh_CN.UTF-8";
  env_buf[idx++] = (char*)"LC_IDENTIFICATION=zh_CN.UTF-8";
  env_buf[idx++] = (char*)"LC_MEASUREMENT=zh_CN.UTF-8";
  env_buf[idx++] = (char*)"LC_MONETARY=zh_CN.UTF-8";
  env_buf[idx++] = (char*)"LC_NAME=zh_CN.UTF-8";
  env_buf[idx++] = (char*)"LC_NUMERIC=zh_CN.UTF-8";
  env_buf[idx++] = (char*)"LC_PAPER=zh_CN.UTF-8";
  env_buf[idx++] = (char*)"LC_TELEPHONE=zh_CN.UTF-8";
  env_buf[idx++] = (char*)"LC_TIME=zh_CN.UTF-8";
  env_buf[idx++] = (char*)"LD_LIBRARY_PATH=/home/nvidia/workspace/wqg/QingLong:/opt/ros/melodic/lib:/usr/local/cuda-10.0/lib64:/opt/miivii/lib:/usr/lib/aarch64-linux-gnu/tegra:./:/opt/ros/melodic/lib:/usr/local/cuda-10.0/lib64:/opt/miivii/lib:/usr/lib/aarch64-linux-gnu/tegra:/opt/ros/melodic/lib:/usr/local/cuda-10.0/lib64:/opt/miivii/lib::/home/nvidia/workspace/wqg/renderdoc/build/bin:/home/nvidia/workspace/wqg/renderdoc/build/bin/../lib:/home/nvidia/workspace/wqg/renderdoc/build/lib";

//30
    env_buf[idx++] = (char*)"LD_PRELOAD=libgtk3-nocsd.so.0:librenderdoc.so";
  env_buf[idx++] = (char*)"LESSCLOSE=/usr/bin/lesspipe %s %s ";
  env_buf[idx++] = (char*)"LESSOPEN=| /usr/bin/lesspipe %s";
  env_buf[idx++] = (char*)"LINES=46";
  env_buf[idx++] = (char*)"LOGNAME=nvidia";
  env_buf[idx++] = (char*)"LS_COLORS=rs=0:di=01;34:ln=01;36:mh=00:pi=40;33:so=01;35:do=01;35:bd=40;33;01:cd=40;33;01:or=40;31;01:mi=00:su=37;41:sg=30;43:ca=30;41:tw=30;42:ow=34;42:st=37;44:ex=01;32:*.tar=01;31:*.tgz=01;31:*.arc=01;31:*.arj=01;31:*.taz=01;31:*.lha=01;31:*.lz4=01;31:*.lzh=01;31:*.lzma=01;31:*.tlz=01;31:*.txz=01;31:*.tzo=01;31:*.t7z=01;31:*.zip=01;31:*.z=01;31:*.Z=01;31:*.dz=01;31:*.gz=01;31:*.lrz=01;31:*.lz=01;31:*.lzo=01;31:*.xz=01;31:*.zst=01;31:*.tzst=01;31:*.bz2=01;31:*.bz=01;31:*.tbz=01;31:*.tbz2=01;31:*.tz=01;31:*.deb=01;31:*.rpm=01;31:*.jar=01;31:*.war=01;31:*.ear=01;31:*.sar=01;31:*.rar=01;31:*.alz=01;31:*.ace=01;31:*.zoo=01;31:*.cpio=01;31:*.7z=01;31:*.rz=01;31:*.cab=01;31:*.wim=01;31:*.swm=01;31:*.dwm=01;31:*.esd=01;31:*.jpg=01;35:*.jpeg=01;35:*.mjpg=01;35:*.mjpeg=01;35:*.gif=01;35:*.bmp=01;35:*.pbm=01;35:*.pgm=01;35:*.ppm=01;35:*.tga=01;35:*.xbm=01;35:*.xpm=01;35:*.tif=01;35:*.tiff=01;35:*.png=01;35:*.svg=01;35:*.svgz=01;35:*.mng=01;35:*.pcx=01;35:*.mov=01;35:*.mpg=01;35:*.mpeg=01;35:*.m2v=01;35:*.mkv=01;35:*.webm=01;35:*.ogm=01;35:*.mp4=01;35:*.m4v=01;35:*.mp4v=01;35:*.vob=01;35:*.qt=01;35:*.nuv=01;35:*.wmv=01;35:*.asf=01;35:*.rm=01;35:*.rmvb=01;35:*.flc=01;35:*.avi=01;35:*.fli=01;35:*.flv=01;35:*.gl=01;35:*.dl=01;35:*.xcf=01;35:*.xwd=01;35:*.yuv=01;35:*.cgm=01;35:*.emf=01;35:*.ogv=01;35:*.ogx=01;35:*.aac=00;36:*.au=00;36:*.flac=00;36:*.m4a=00;36:*.mid=00;36:*.midi=00;36:*.mka=00;36:*.mp3=00;36:*.mpc=00;36:*.ogg=00;36:*.ra=00;36:*.wav=00;36:*.oga=00;36:*.opus=00;36:*.spx=00;36:*.xspf=00;36:";
  env_buf[idx++] = (char*)"MANAGERPID=6563";
  env_buf[idx++] = (char*)"OLDPWD=/home/nvidia";
  env_buf[idx++] = (char*)"PATH=/opt/ros/melodic/bin:/usr/local/cuda-10.0/bin:/opt/miivii/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/games:/usr/local/games:/snap/bin:/home/nvidia/workspace/wqg/QingLong";
  env_buf[idx++] = (char*)"PKG_CONFIG_PATH=/opt/ros/melodic/lib/pkgconfig";

//40
    env_buf[idx++] = (char*)"PWD=/home/nvidia/workspace/wqg/renderdoc/build/bin";
  env_buf[idx++] = (char*)"PYTHONPATH=/opt/ros/melodic/lib/python2.7/dist-packages:/usr/local/python:";
  env_buf[idx++] = (char*)"QT4_IM_MODULE=xim";
  env_buf[idx++] = (char*)"QT_ACCESSIBILITY=1";
  env_buf[idx++] = (char*)"QT_IM_MODULE=ibus";
  env_buf[idx++] = (char*)"RENDERDOC_CAPFILE= ";
  env_buf[idx++] = (char*)"RENDERDOC_CAPOPTS=ababaaaaaaaaaaaaaaaaaaaaaaaaaaaaabohpppp";
  env_buf[idx++] = (char*)"RENDERDOC_DEBUG_LOG_FILE=/tmp/RenderDoc/RenderDoc_2022.12.05_15.04.29.log";
  env_buf[idx++] = (char*)"RENDERDOC_ORIGLIBPATH=/home/nvidia/workspace/wqg/QingLong:/opt/ros/melodic/lib:/usr/local/cuda-10.0/lib64:/opt/miivii/lib:/usr/lib/aarch64-linux-gnu/tegra:./:/opt/ros/melodic/lib:/usr/local/cuda-10.0/lib64:/opt/miivii/lib:/usr/lib/aarch64-linux-gnu/tegra:/opt/ros/melodic/lib:/usr/local/cuda-10.0/lib64:/opt/miivii/lib:";
  env_buf[idx++] = (char*)"RENDERDOC_ORIGPRELOAD=libgtk3-nocsd.so.0";

//50
    env_buf[idx++] = (char*)"ROSLISP_PACKAGE_DIRECTORIES= ";
  env_buf[idx++] = (char*)"ROS_DISTRO=melodic";
  env_buf[idx++] = (char*)"ROS_ETC_DIR=/opt/ros/melodic/etc/ros";
  env_buf[idx++] = (char*)"ROS_MASTER_URI=http://localhost:11311";
  env_buf[idx++] = (char*)"ROS_PACKAGE_PATH=/opt/ros/melodic/share";
  env_buf[idx++] = (char*)"ROS_PYTHON_VERSION=2";
  env_buf[idx++] = (char*)"ROS_ROOT=/opt/ros/melodic/share/ros";
  env_buf[idx++] = (char*)"ROS_VERSION=1";
  env_buf[idx++] = (char*)"SHELL=/bin/bash";
  env_buf[idx++] = (char*)"SHLVL=2";

//60
    env_buf[idx++] = (char*)"SSH_AGENT_LAUNCHER=gnome-keyring";
  env_buf[idx++] = (char*)"SSH_AUTH_SOCK=/run/user/1000/keyring/ssh";
  env_buf[idx++] = (char*)"TERM=xterm-256color";
  env_buf[idx++] = (char*)"TEXTDOMAIN=im-config";
  env_buf[idx++] = (char*)"TEXTDOMAINDIR=/usr/share/locale/";
  env_buf[idx++] = (char*)"USER=nvidia";
  env_buf[idx++] = (char*)"USERNAME=nvidia";
  env_buf[idx++] = (char*)"VTE_VERSION=5202";
  env_buf[idx++] = (char*)"WINDOWPATH=1";
  env_buf[idx++] = (char*)"XAUTHORITY=/run/user/1000/gdm/Xauthority";

//70
    env_buf[idx++] = (char*)"XDG_CONFIG_DIRS=/etc/xdg/xdg-unity:/etc/xdg";
  env_buf[idx++] = (char*)"XDG_CURRENT_DESKTOP=Unity:Unity7:ubuntu";
  env_buf[idx++] = (char*)"XDG_DATA_DIRS=/usr/share/unity:/home/nvidia/.local/share/flatpak/exports/share/:/var/lib/flatpak/exports/share/:/usr/local/share/:/usr/share/:/var/lib/snapd/desktop";
  env_buf[idx++] = (char*)"XDG_RUNTIME_DIR=/run/user/1000";
  env_buf[idx++] = (char*)"XDG_SESSION_DESKTOP=unity";
  env_buf[idx++] = (char*)"XDG_SESSION_TYPE=x11";
  env_buf[idx++] = (char*)"XMODIFIERS=@im=ibus";
  env_buf[idx++] = (char*)"_=./renderdoccmd";
  env_buf[idx] = 0;

  printf("env_buf cout = %d \n ", idx);

  execve(hmi, argv_buf, env_buf);
#elif 1
    char * args[] = {(char*) "/home/nvidia/workspace/wqg/QingLong/hmi",(char*) NULL };  
  	std::cout << "argc = " << argc << std::endl;
    std::cout << "-------------------BEGIN OUTPUT ARGV-----------------" << std::endl;
    int i = 0;
    while(argv[i])
    {
	    std::cout << i << ">>" << argv[i] << std::endl;
	    i++;
    }
    std::cout << "--------------END ARGV------ BEGIN OUT PUT ENV----------------" << std::endl;
	  int j = 0;
    while(penv[j])
    {
	    std::cout << penv[j++] << std::endl;
    }
    std::cout << "===========END ENV===========" << std::endl;
    const int LEN = 256;
    // {
    //   int number = 0;
    //   while (penv[number] != 0)
    //   {
    //     number ++;
    //   }
    //   if (number > 0)
    //   {

    //     for (int i = 0; i < number; i++)
    //     {
    //       global_envp[i] = new char[LEN];
    //       memset(&global_envp[i], 0, LEN);
    //       int len = strlen(&(penv[i][0]));
    //       printf("len = %d\n", len);
    //       memcpy(&global_envp[i], &penv[i], len);
    //     }
    //     global_envp[number] = 0;
    //   }
    // }
// #endif 
//   #ifndef USE_RENDER_DOC_CMD
    if(argc > 1)
    {
      pid_t childPid = 0;
      childPid = fork();
      if(childPid == 0)
	    {
        std::cout << "argv[1] = " << argv[1] << std::endl;
        const char *workdir = "/home/nvidia/workspace/wqg/QingLong/";
        chdir(workdir);
        std::cout << "change workdir = " << workdir << std::endl;
        int ret = execve(argv[1], &argv[1], penv); 
        if (  -1 == ret )  
        { 
          std::cout << "failed to execve" << std::endl; 
            perror( "execve" );  
            exit( EXIT_FAILURE);  
        }
        else
        {
          std::cout << "success to execve " << workdir << argv[1] << std::endl;
        }
      }
      else
      {
        std::cout << "main create childPid = " << childPid << std::endl;
      }
    }
    puts( "shouldn't get here" );  
    exit( EXIT_SUCCESS );  
  return 0;
#endif

#ifdef USE_RENDER_DOC_CMD
  setlocale(LC_CTYPE, "");

  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);

  GlobalEnvironment env;

#if defined(RENDERDOC_WINDOWING_XLIB) || defined(RENDERDOC_WINDOWING_XCB)
  // call XInitThreads - although we don't use xlib concurrently the driver might need to.
  XInitThreads();

  // we don't check if display successfully opened, it's only a problem if it's needed later.
  display = env.xlibDisplay = XOpenDisplay(NULL);
#endif

  // add compiled-in support to version line
  {
    std::string support = "APIs supported at compile-time: ";
    int count = 0;

#if defined(RENDERDOC_SUPPORT_VULKAN)
    support += "Vulkan, ";
    count++;
#endif

#if defined(RENDERDOC_SUPPORT_GL)
    support += "GL, ";
    count++;
#endif

#if defined(RENDERDOC_SUPPORT_GLES)
    support += "GLES, ";
    count++;
#endif

    if(count == 0)
    {
      support += "None.";
    }
    else
    {
      // remove trailing ', '
      support.pop_back();
      support.pop_back();
      support += ".";
    }

    add_version_line(support);

    support = "Windowing systems supported at compile-time: ";
    count = 0;

#if defined(RENDERDOC_WINDOWING_XLIB)
    support += "xlib, ";
    count++;
#endif

#if defined(RENDERDOC_WINDOWING_XCB)
    support += "XCB, ";
    count++;
#endif

#if defined(RENDERDOC_WINDOWING_WAYLAND)
    support += "Wayland (CAPTURE ONLY), ";
    count++;
#endif

#if defined(RENDERDOC_SUPPORT_VULKAN)
    support += "Vulkan KHR_display, ";
    count++;
#endif

    if(count == 0)
    {
      support += "None.";
    }
    else
    {
      // remove trailing ', '
      support.pop_back();
      support.pop_back();
      support += ".";
    }

    add_version_line(support);
  }

  int ret = renderdoccmd(env, argc, argv);

#if defined(RENDERDOC_WINDOWING_XLIB) || defined(RENDERDOC_WINDOWING_XCB)
  if(display)
    XCloseDisplay(display);
#endif
return ret;
#endif //USE_RENDER_DOC_CMD
}
