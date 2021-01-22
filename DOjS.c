/*
MIT License

Copyright (c) 2019-2021 Andre Seidelt <superilu@yahoo.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "DOjS.h"

#include <conio.h>
#include <glide.h>
#include <jsi.h>
#include <dos.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <strings.h>
#include <dlfcn.h>

#include "3dfx-glide.h"
#include "3dfx-state.h"
#include "3dfx-texinfo.h"
#include "bitmap.h"
#include "color.h"
#include "edit.h"
#include "file.h"
#include "font.h"
#include "funcs.h"
#include "gfx.h"
#include "joystick.h"
#include "midiplay.h"
#include "socket.h"
#include "sound.h"
#include "util.h"
#include "watt.h"
#include "zip.h"
#include "zipfile.h"
#include "lowlevel.h"
#include "intarray.h"

#define TICK_DELAY 10  //!< system tick handler interval in ms
#define AUTOSTART_FILE "=MAIN.JS"

/**************
** Variables **
**************/
static int last_mouse_x;
static int last_mouse_y;
static int last_mouse_b;

dojs_t DOjS;  //!< global data structure

/************************
** function prototypes **
************************/
static void tick_handler(void);
static void tick_handler_end(void);

/*********************
** static functions **
*********************/
static void tick_handler() { DOjS.sys_ticks += TICK_DELAY; }
END_OF_FUNCTION(tick_handler)

/**
 * @brief show usage on console
 */
static void usage() {
    fputs("Usage: DOjS.EXE [-r] [-l] [-s] [-f] [-a] <script> [script parameters]\n", stderr);
    fputs("    -r             : Do not invoke the editor, just run the script.\n", stderr);
    fputs("    -l             : Use 50-line mode in the editor.\n", stderr);
    fputs("    -w <width>     : Screen width: 320 or 640, Default: 640.\n", stderr);
    fputs("    -b <bpp>       : Bit per pixel:8, 16, 24, 32. Default: 32.\n", stderr);
    fputs("    -s             : No wave sound.\n", stderr);
    fputs("    -f             : No FM sound.\n", stderr);
    fputs("    -a             : Disable alpha (speeds up rendering).\n", stderr);
    fputs("    -x             : Allow raw disk write (CAUTION!)\n", stderr);
    fputs("\n", stderr);
    fputs("This is DOjS " DOSJS_VERSION_STR "\n", stderr);
    fputs("(c) 2019-2021 by Andre Seidelt <superilu@yahoo.com> and others.\n", stderr);
    fputs("See LICENSE for detailed licensing information.\n", stderr);
    fputs("\n", stderr);
    exit(1);
}

/**
 * @brief write panic message.
 *
 * @param J VM state.
 */
static void Panic(js_State *J) { LOGF("!!! PANIC in %s !!!\n", J->filename); }

/**
 * @brief write 'report' message.
 *
 * @param J VM state.
 */
static void Report(js_State *J, const char *message) {
    DOjS.lastError = message;
    LOGF("%s\n", message);
}

/**
 * @brief call a globally define JS function.
 *
 * @param J VM state.
 * @param name function name.
 *
 * @return true if the function was found.
 * @return false if the function was not found.
 */
static bool callGlobal(js_State *J, const char *name) {
    js_getglobal(J, name);
    js_pushnull(J);
    if (js_pcall(J, 0)) {
        DOjS.lastError = js_trystring(J, -1, "Error");
        LOGF("Error calling %s: %s\n", name, DOjS.lastError);
        return false;
    }
    js_pop(J, 1);
    return true;
}

/**
 * @brief handle input.
 *
 * @param J VM state.
 *
 * @return true if the found event was exit_key.
 * @return false if no or any other event occured.
 */
static bool callInput(js_State *J) {
    int key;
    bool ret = false;

    if (keyboard_needs_poll()) {
        poll_keyboard();
    }
    if (mouse_needs_poll()) {
        poll_mouse();
    }

    if (keypressed()) {
        key = readkey();
        ret = ((key >> 8) == DOjS.exit_key);
    } else {
        key = -1;
        ret = FALSE;
    }

    // do not call JS if nothing changed
    if (key == -1 && last_mouse_x == mouse_x && last_mouse_y == mouse_y && last_mouse_b == mouse_b) {
        return ret;
    }

    // store new values
    last_mouse_x = mouse_x;
    last_mouse_y = mouse_y;
    last_mouse_b = mouse_b;

    // call JS
    js_getglobal(J, CB_INPUT);
    js_pushnull(J);
    js_newobject(J);
    {
        js_pushnumber(J, mouse_x);
        js_setproperty(J, -2, "x");
        js_pushnumber(J, mouse_y);
        js_setproperty(J, -2, "y");
        js_pushnumber(J, mouse_b);
        js_setproperty(J, -2, "buttons");
        js_pushnumber(J, key);
        js_setproperty(J, -2, "key");
        js_pushnumber(J, DOjS.sys_ticks);
        js_setproperty(J, -2, "ticks");
    }
    if (js_pcall(J, 1)) {
        DOjS.lastError = js_trystring(J, -1, "Error");
        LOGF("Error calling Input(): %s\n", DOjS.lastError);
    }
    js_pop(J, 1);

    return ret;
}

/**
 * @brief alpha calculation function used with transparent colors.
 * see https://www.gamedev.net/forums/topic/34688-alpha-blend-formula/
 *
 * @param src the new color with the alpha information.
 * @param dest the existing color in the BITMAP.
 * @param n ignored.
 * @return unsigned long the color to put into the BITMAP.
 */
static unsigned long my_blender(unsigned long src, unsigned long dest, unsigned long n) {
    int a = (src >> 24) & 0xFF;
    if (a >= 254) {
        return src;  // no alpha, just return new color
    } else {
        int r1 = (src >> 16) & 0xFF;
        int g1 = (src >> 8) & 0xFF;
        int b1 = src & 0xFF;

        int r2 = (dest >> 16) & 0xFF;
        int g2 = (dest >> 8) & 0xFF;
        int b2 = dest & 0xFF;

        unsigned long ret = 0xFF000000 |                                          // alpha
                            (((((a * (r1 - r2)) >> 8) + r2) << 16) & 0xFF0000) |  // new r
                            (((((a * (g1 - g2)) >> 8) + g2) << 8) & 0x00FF00) |   // new g
                            ((((a * (b1 - b2)) >> 8) + b2) & 0x0000FF);           // new b
        // DEBUGF("0x%08lX 0x%08lX 0x%08lX 0x%08lX\n", src, dest, n, ret);
        return ret;
    }
}

/**
 * @brief load and parse a javascript file from ZIP.
 *
 * @param J VM state.
 * @param fname fname, ZIP-files using ZIP_DELIM.
 */
static void dojs_loadfile_zip(js_State *J, const char *fname) {
    char *s, *p;
    size_t n;

    if (!read_zipfile1(fname, (void **)&s, &n)) {
        js_error(J, "cannot open file '%s'", fname);
        return;
    }

    if (js_try(J)) {
        free(s);
        js_throw(J);
    }

    /* skip first line if it starts with "#!" */
    p = s;
    if (p[0] == '#' && p[1] == '!') {
        p += 2;
        while (*p && *p != '\n') ++p;
    }

    DEBUGF("Parsing ZIP entry '%s'\n", fname);

    js_loadstring(J, fname, p);

    free(s);
    js_endtry(J);
}

/**
 * @brief load all system include JS files either from ZIP (if found) or from file system.
 *
 * @param J VM state.
 */
static void dojs_load_jsboot(js_State *J) {
    if (ut_file_exists(JSBOOT_ZIP)) {
        DEBUG("JSBOOT.ZIP found, using archive\n");
        PROPDEF_S(J, JSBOOT_ZIP ZIP_DELIM_STR JSBOOT_DIR, JSBOOT_VAR);
        dojs_do_file(J, JSBOOT_ZIP ZIP_DELIM_STR JSINC_FUNC);
        dojs_do_file(J, JSBOOT_ZIP ZIP_DELIM_STR JSINC_COLOR);
        dojs_do_file(J, JSBOOT_ZIP ZIP_DELIM_STR JSINC_FILE);
        dojs_do_file(J, JSBOOT_ZIP ZIP_DELIM_STR JSINC_3DFX);
        dojs_do_file(J, JSBOOT_ZIP ZIP_DELIM_STR JSINC_SOCKET);
    } else {
        DEBUG("JSBOOT.ZIP NOT found, using plain files\n");
        PROPDEF_S(J, JSBOOT_DIR, JSBOOT_VAR);
        dojs_do_file(J, JSINC_FUNC);
        dojs_do_file(J, JSINC_COLOR);
        dojs_do_file(J, JSINC_FILE);
        dojs_do_file(J, JSINC_3DFX);
        dojs_do_file(J, JSINC_SOCKET);
    }
}

#ifdef MEMDEBUG
/**
 * @brief debugging version of js_alloc()
 *
 * @param actx context (unused).
 * @param ptr pointer for remalloc()/free()
 * @param size realloc()/malloc() size, 0 for free().
 *
 * @return void* (re)allocated memory
 */
static void *dojs_alloc(void *actx, void *ptr, int size) {
    void *ret = NULL;
    if (size == 0) {
        free(ptr);
        ret = NULL;
        DEBUGF("DBG FREE(0x%p, %d) := 0x%p\n", ptr, size, ret);
    } else if (!ptr) {
        DOjS.num_allocs++;
        ret = malloc((size_t)size);
        DEBUGF("DBG MALL(0x%p, %d) := 0x%p\n", ptr, size, ret);
        // if (!ret) {
        //     LOGF("MALLOC failed");
        //     exit(1);
        // }
    } else {
        DOjS.num_allocs++;
        ret = realloc(ptr, (size_t)size);
        DEBUGF("DBG   RE(0x%p, %d) := 0x%p\n", ptr, size, ret);
        // if (!ret) {
        //     LOGF("REALLOC failed");
        //     exit(1);
        // }
    }
    return ret;
}
#else
/**
 * @brief debugging version of js_alloc()
 *
 * @param actx context (unused).
 * @param ptr pointer for remalloc()/free()
 * @param size realloc()/malloc() size, 0 for free().
 *
 * @return void* (re)allocated memory
 */
static void *dojs_alloc(void *actx, void *ptr, int size) {
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    DOjS.num_allocs++;
    return realloc(ptr, (size_t)size);
}
#endif

/**
 * @brief call shutdown() on all registered libraries
 */
static void dojs_shutdown_libraries() {
    if (DOjS.loaded_libraries) {
        library_t *chain = DOjS.loaded_libraries;
        while (chain) {
            DEBUGF("%p: Library shutdown for %s. Shutdown function %p\n", chain, chain->name, chain->shutdown);

            // call shutdown if any
            if (chain->shutdown) {
                chain->shutdown();
            }
            chain = chain->next;
        }
    }
}

/**
 * @brief run the given script.
 *
 * @param argc number of parameters.
 * @param argv command line parameters.
 * @param args first script parameter.
 */
static void run_script(int argc, char **argv, int args) {
    js_State *J;

    // create logfile
    DOjS.logfile = fopen(LOGFILE, "a");
    setbuf(DOjS.logfile, 0);
#ifdef DEBUG_ENABLED
    freopen("STDOUT.DJS", "a", stdout);
    freopen("STDERR.DJS", "a", stderr);
#endif

    // (re)init out DOjS struct
    DOjS.num_allocs = 0;
    DOjS.exit_key = KEY_ESC;  // the exit key that will stop the script
    DOjS.sys_ticks = 0;
    DOjS.glide_enabled = false;
    DOjS.mouse_available = false;
    DOjS.mouse_visible = false;
    DOjS.lastError = NULL;

    // create VM
    J = js_newstate(dojs_alloc, NULL, 0);
    js_atpanic(J, Panic);
    js_setreport(J, Report);

    // write startup message
    LOG("-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n");
    LOGF("DOjS %s (%s %s) starting with file %s\n", DOSJS_VERSION_STR, __DATE__, __TIME__, DOjS.params.script);
    DEBUGF("Running on %s %d.%d\n", _os_flavor, _osmajor, _osminor);
#ifdef DEBUG_ENABLED
    // ut_dumpVideoModes();
#endif

    // detect hardware and initialize subsystems
    allegro_init();
    install_timer();
    LOCK_VARIABLE(DOjS.sys_ticks);
    LOCK_FUNCTION(tick_handler);
    install_int(tick_handler, TICK_DELAY);
    install_keyboard();
    if (install_mouse() >= 0) {
        LOG("Mouse detected\n");
        enable_hardware_cursor();
        select_mouse_cursor(MOUSE_CURSOR_ARROW);
        DOjS.mouse_available = true;
        DOjS.mouse_visible = true;
    } else {
        LOGF("NO Mouse detected: %s\n", allegro_error);
    }
    PROPDEF_B(J, DOjS.mouse_available, "MOUSE_AVAILABLE");
    init_sound(J);  // sound init must be before midi init!
    init_midi(J);
    init_funcs(J, argc, argv, args);  // must be called after initalizing the booleans above!
    init_lowlevel(J);
    init_gfx(J);
    init_color(J);
    init_bitmap(J);
    init_font(J);
    init_file(J);
    init_3dfx(J);
    init_texinfo(J);
    init_fxstate(J);
    init_joystick(J);
    init_watt(J);
    init_socket(J);
    init_zipfile(J);
    init_intarray(J);

    // create canvas
    bool screenSuccess = true;
    while (true) {
        set_color_depth(DOjS.params.bpp);
        if (DOjS.params.width == 640) {
            if (set_gfx_mode(GFX_AUTODETECT, 640, 480, 0, 0) != 0) {
                LOGF("Couldn't set a %d bit color resolution at 640x480: %s\n", DOjS.params.bpp, allegro_error);
            } else {
                break;
            }
        } else {
            if (set_gfx_mode(GFX_AUTODETECT, 320, 240, 0, 0) != 0) {
                LOGF("Couldn't set a %d bit color resolution at 320x240: %s\n", DOjS.params.bpp, allegro_error);
            } else {
                break;
            }
        }
        if (DOjS.params.bpp == 32) {
            DOjS.params.bpp = 24;
            LOG("32 bit color resolution not available, trying 24 bit fallback...\n");
        } else {
            screenSuccess = false;
            break;
        }
    }
    if (DOjS.params.bpp < 24) {
        DOjS.params.no_alpha = true;
        LOG("BPP < 24, disabling alpha\n");
    }
    if (screenSuccess) {
        DOjS.render_bm = DOjS.current_bm = create_bitmap(SCREEN_W, SCREEN_H);
        clear_bitmap(DOjS.render_bm);
        DOjS.transparency_available = !DOjS.params.no_alpha;
        dojs_update_transparency();

        DEBUGF("GFX_Capabilities=%08X\n", gfx_capabilities);

        // do some more init from JS
        dojs_load_jsboot(J);

        // load main file
        if (dojs_do_file(J, DOjS.params.script) == 0) {
            // call setup()
            DOjS.keep_running = true;
            DOjS.wanted_frame_rate = 30;
            if (callGlobal(J, CB_SETUP)) {
                // call loop() until someone calls Stop()
                while (DOjS.keep_running) {
                    long start = DOjS.sys_ticks;
                    if (DOjS.num_allocs > 1000) {
#ifdef MEMDEBUG
                        js_gc(J, 1);
#else
                        js_gc(J, 0);
#endif
                        DOjS.num_allocs = 0;
                    }
                    tick_socket();
                    if (!callGlobal(J, CB_LOOP)) {
                        if (!DOjS.lastError) {
                            DOjS.lastError = "Loop() not found.";
                        }
                        break;
                    }
                    if (callInput(J)) {
                        DOjS.keep_running = false;
                    }
                    if (DOjS.glide_enabled) {
                        grBufferSwap(1);
                    } else {
                        blit(DOjS.render_bm, screen, 0, 0, 0, 0, SCREEN_W, SCREEN_H);
                        if (DOjS.mouse_visible) {
                            show_mouse(screen);
                        }
                    }
                    long end = DOjS.sys_ticks;
                    long runtime = (end - start) + 1;
                    DOjS.current_frame_rate = 1000 / runtime;
                    if (DOjS.current_frame_rate > DOjS.wanted_frame_rate) {
                        unsigned int delay = (1000 / DOjS.wanted_frame_rate) - runtime;
                        rest(delay);
                    }
                    end = DOjS.sys_ticks;
                    runtime = (end - start) + 1;
                    DOjS.current_frame_rate = 1000 / runtime;
                }
            } else {
                if (!DOjS.lastError) {
                    DOjS.lastError = "Setup() not found.";
                }
            }
        }
    } else {
        if (!DOjS.lastError) {
            DOjS.lastError = "Screen resolution and depth now available.";
        }
    }
    LOG("DOjS Shutdown...\n");
    js_freestate(J);
    dojs_shutdown_libraries();
    shutdown_midi();
    shutdown_sound();
    shutdown_joystick();
    shutdown_3dfx();
    fclose(DOjS.logfile);
    allegro_exit();
    textmode(C80);
    if (DOjS.lastError) {
        fputs(DOjS.lastError, stdout);
        fputs("\nDOjS ERROR\n", stdout);
    } else {
        fputs("DOjS OK\n", stdout);
    }
    fflush(stdout);
}

/***********************
** exported functions **
***********************/
/**
 * @brief load and parse a file from filesystem or ZIP.
 *
 * @param J VM state.
 * @param fname filename, ZIP-files using ZIP_DELIM.
 *
 * @return int TRUE if successfull, FALSE if not.
 */
int dojs_do_file(js_State *J, const char *fname) {
    char *delim = strchr(fname, ZIP_DELIM);
    if (!delim) {
        DEBUGF("Parsing plain file '%s'\n", fname);
        return js_dofile(J, fname);
    } else {
        if (js_try(J)) {
            js_report(J, js_trystring(J, -1, "Error"));
            js_pop(J, 1);
            return 1;
        }
        dojs_loadfile_zip(J, fname);
        js_pushundefined(J);
        js_call(J, 0);
        js_pop(J, 1);
        js_endtry(J);
        return 0;
    }
}

/**
 * @brief register a library.
 *
 * @param name pointer to a name. This function will make a copy of the string.
 * @param handle the handle returned by dlopen().
 * @param init the init function.
 * @param shutdown function to be called for shutdown or NULL.
 *
 * @return true if registration succeeded, else false.
 */
bool dojs_register_library(const char *name, void *handle, void (*init)(js_State *J), void (*shutdown)(void)) {
    DEBUGF("Registering library %s. Shutdown function %p\n", name, shutdown);

    // get new entry
    library_t *new_entry = calloc(1, sizeof(library_t));
    if (!new_entry) {
        LOGF("WARNING: Could not register shutdown hook for loaded library %s!", name);
        return false;
    }

    // copy name
    char *name_copy = malloc(strlen(name) + 1);
    if (!name_copy) {
        LOGF("WARNING: Could not register shutdown hook for loaded library %s!", name);
        free(new_entry);
        return false;
    }
    strcpy(name_copy, name);

    // store values
    new_entry->name = name_copy;
    new_entry->handle = handle;
    new_entry->init = init;
    new_entry->shutdown = shutdown;
    DEBUGF("%s: Created %p with init=%p and shutdown=%p\n", new_entry->name, new_entry, new_entry->init, new_entry->shutdown);

    // insert at start of list
    new_entry->next = DOjS.loaded_libraries;
    DOjS.loaded_libraries = new_entry;
    return true;
}

/**
 * @brief check if a given library is already registered. Call init() if wanted
 *
 * @param J VM state.
 * @param name name to search for.
 * @param call_init true to call the init function, else false.
 * @return true if the librari is already in the list, else false.
 */
bool dojs_check_library(js_State *J, const char *name, bool call_init) {
    if (DOjS.loaded_libraries) {
        library_t *chain = DOjS.loaded_libraries;
        while (chain) {
            if (strcmp(name, chain->name) == 0) {
                DEBUGF("%s: Found %p\n", chain->name, chain);
                if (call_init) {
                    DEBUGF("%s: Calling %p\n", chain->name, chain->init);
                    chain->init(J);
                }
                return true;
            }

            chain = chain->next;
        }
    }
    return false;
}

void dojs_update_transparency() {
    if (DOjS.transparency_available) {
        set_blender_mode(my_blender, my_blender, my_blender, 0, 0, 0, 0);
        drawing_mode(DRAW_MODE_TRANS, DOjS.render_bm, 0, 0);
    } else {
        drawing_mode(DRAW_MODE_SOLID, DOjS.render_bm, 0, 0);
    }
}

/**
 * @brief main entry point.
 *
 * @param argc number of parameters.
 * @param argv command line parameters.
 *
 * @return int exit code.
 */
int main(int argc, char **argv) {
    // initialize DOjS main structure
    bzero(&DOjS, sizeof(DOjS));
    DOjS.params.width = 640;
    DOjS.params.bpp = 32;
    int opt;

    // check parameters
    while ((opt = getopt(argc, argv, "xlrsfahw:b:")) != -1) {
        switch (opt) {
            case 'w':
                DOjS.params.width = atoi(optarg);
                break;
            case 'b':
                DOjS.params.bpp = atoi(optarg);
                break;
            case 'r':
                DOjS.params.run = true;
                break;
            case 'l':
                DOjS.params.highres = true;
                break;
            case 's':
                DOjS.params.no_sound = true;
                break;
            case 'f':
                DOjS.params.no_fm = true;
                break;
            case 'a':
                DOjS.params.no_alpha = true;
                break;
            case 'x':
                DOjS.params.raw_write = true;
                break;
            case 'h':
            default: /* '?' */
                usage();
                exit(EXIT_FAILURE);
        }
    }

    if (optind >= argc) {
        // no script name supplied, try autostart with xxx.EXE->xxx.ZIP and JSBOOT.ZIP
        int autostart_len = strlen(argv[0]);
        char *autostart_zip = malloc(autostart_len + 1);
        if (!autostart_zip) {
            fprintf(stderr, "Out of memory.\n\n");
            exit(EXIT_FAILURE);
        }
        strcpy(autostart_zip, argv[0]);
        autostart_zip[autostart_len - 3] = 'Z';
        autostart_zip[autostart_len - 2] = 'I';
        autostart_zip[autostart_len - 1] = 'P';
        autostart_zip[autostart_len] = 0;
        // try autostart
        char *autostart_script = malloc(autostart_len + 1 + strlen(AUTOSTART_FILE));
        if (!autostart_script) {
            fprintf(stderr, "Out of memory.\n\n");
            exit(EXIT_FAILURE);
        }
        strcpy(autostart_script, autostart_zip);
        strcat(autostart_script, AUTOSTART_FILE);
        free(autostart_zip);
        if (check_zipfile1(autostart_script)) {
            // we found a ZIP file with the name of the EXE and it has a MAIN.JS
            DOjS.params.script = autostart_script;
            DOjS.params.run = true;
        } else if (check_zipfile1(JSBOOT_ZIP AUTOSTART_FILE)) {
            // we found MAIN.JS inside JSBOOT.ZIP
            DOjS.params.script = JSBOOT_ZIP AUTOSTART_FILE;
            DOjS.params.run = true;
        }
    } else {
        // script is normal command line parameters
        DOjS.params.script = argv[optind];
    }

    // check if the above yielded a script name and if the combination is valid
    if (!DOjS.params.script) {
        fprintf(stderr, "Script name missing.\n\n");
        usage();
        exit(EXIT_FAILURE);
    }
    if (!DOjS.params.run && strchr(DOjS.params.script, ZIP_DELIM)) {
        fprintf(stderr, "ZIP-Scripts are only supported with option '-r'.\n\n");
        usage();
        exit(EXIT_FAILURE);
    }

    // check screen size parameters
    if (DOjS.params.width != 640 && DOjS.params.width != 320) {
        fprintf(stderr, "Screen width must be 640 or 320 pixel, not %d.\n\n", DOjS.params.width);
        usage();
        exit(EXIT_FAILURE);
    }

    // check bpp parameters
    if (DOjS.params.bpp != 8 && DOjS.params.bpp != 16 && DOjS.params.bpp != 24 && DOjS.params.bpp != 32) {
        fprintf(stderr, "Bits per pixel must be 8, 16, 24 or 32 pixel, not %d.\n\n", DOjS.params.bpp);
        usage();
        exit(EXIT_FAILURE);
    }

    // ignore ctrl-c, we need it in the editor!
    signal(SIGINT, SIG_IGN);

    while (true) {
        edi_exit_t exit = EDI_KEEPRUNNING;
        if (!DOjS.params.run) {
            exit = edi_edit(DOjS.params.script, DOjS.params.highres);
        } else {
            exit = EDI_RUNSCRIPT;
        }

        if (exit == EDI_RUNSCRIPT) {
            run_script(argc, argv, optind);
        }

        if (DOjS.params.run || exit == EDI_QUIT || exit == EDI_ERROR) {
            break;
        }
    }

    exit(0);
}
END_OF_MAIN()
