/*
MIT License

Copyright (c) 2019-2020 Andre Seidelt <superilu@yahoo.com>

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

#include <errno.h>
#include <mujs.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "DOjS.h"
#include "file.h"

/************
** defines **
************/
#define MAX_LINE_LENGTH 4096  //!< read at max 4KiB

/************
** structs **
************/
//! file userdata definition
typedef struct __file {
    FILE *file;      //!< the file pointer
    bool writeable;  //!< indicates the file was opened for writing
} file_t;

/*********************
** static functions **
*********************/
/**
 * @brief finalize a file and free resources.
 *
 * @param J VM state.
 */
static void File_Finalize(js_State *J, void *data) {
    file_t *f = (file_t *)data;
    if (f->file) {
        fclose(f->file);
    }
    free(f);
}

/**
 * @brief open a file and store it as userdata in JS object.
 * new File(filename:string, mode:string)
 *
 * @param J VM state.
 */
static void new_File(js_State *J) {
    NEW_OBJECT_PREP(J);
    const char *fname = js_tostring(J, 1);

    file_t *f = malloc(sizeof(file_t));
    if (!f) {
        JS_ENOMEM(J);
        return;
    }

    const char *mode = js_tostring(J, 2);
    if (mode[0] == 'a' || mode[0] == 'w') {
        f->writeable = true;
    } else if (mode[0] == 'r') {
        f->writeable = false;
    } else {
        js_error(J, "Unknown mode for file '%s'", mode);
        free(f);
        return;
    }

    f->file = fopen(fname, mode);
    if (!f->file) {
        js_error(J, "cannot open file '%s': %s", fname, strerror(errno));
    }

    js_currentfunction(J);
    js_getproperty(J, -1, "prototype");
    js_newuserdata(J, TAG_FILE, f, File_Finalize);
}

/**
 * @brief return the next byte from the file as number.
 * file.ReadByte():number
 *
 * @param J VM state.
 */
static void File_ReadByte(js_State *J) {
    file_t *f = js_touserdata(J, 0, TAG_FILE);
    if (!f->file) {
        js_error(J, "File was closed!");
        return;
    }

    if (f->writeable) {
        js_error(J, "File was opened for writing!");
        return;
    } else {
        int ch = getc(f->file);
        if (ch != EOF) {
            js_pushnumber(J, ch);
        } else {
            js_pushnull(J);
        }
    }
}

/**
 * @brief return the remaining bytes from the file as number array.
 * file.ReadBytes():number[]
 *
 * @param J VM state.
 */
static void File_ReadBytes(js_State *J) {
    file_t *f = js_touserdata(J, 0, TAG_FILE);
    if (!f->file) {
        js_error(J, "File was closed!");
        return;
    }

    if (f->writeable) {
        js_error(J, "File was opened for writing!");
        return;
    } else {
        js_newarray(J);

        int idx = 0;
        int ch = getc(f->file);
        while (ch != EOF) {
            js_pushnumber(J, ch);
            js_setindex(J, -2, idx);
            idx++;
            ch = getc(f->file);
        }
    }
}

/**
 * @brief return the next line from the file as string.
 * file.ReadLine():string
 *
 * @param J VM state.
 */
static void File_ReadLine(js_State *J) {
    file_t *f = js_touserdata(J, 0, TAG_FILE);
    if (!f->file) {
        js_error(J, "File was closed!");
        return;
    }

    if (f->writeable) {
        js_error(J, "File was opened for writing!");
        return;
    } else {
        char line[MAX_LINE_LENGTH + 1];
        char *s = fgets(line, sizeof(line), f->file);
        if (s) {
            js_pushstring(J, line);
        } else {
            js_pushnull(J);
        }
    }
}

/**
 * @brief close the file.
 * file.Close()
 *
 * @param J VM state.
 */
static void File_Close(js_State *J) {
    file_t *f = js_touserdata(J, 0, TAG_FILE);
    if (f->file) {
        fclose(f->file);
        f->file = NULL;
    }
}

/**
 * @brief get size of file.
 * file.GetSize():number
 *
 * @param J VM state.
 */
static void File_GetSize(js_State *J) {
    file_t *f = js_touserdata(J, 0, TAG_FILE);
    if (!f->file) {
        js_error(J, "File was closed!");
        return;
    }

    int old = ftell(f->file);
    fseek(f->file, 0, SEEK_END);
    int size = ftell(f->file);
    fseek(f->file, old, SEEK_SET);

    js_pushnumber(J, size);
}

/**
 * @brief write a byte to a file.
 * file.WriteByte(ch:number)
 *
 * @param J VM state.
 */
static void File_WriteByte(js_State *J) {
    file_t *f = js_touserdata(J, 0, TAG_FILE);
    if (!f->file) {
        js_error(J, "File was closed!");
        return;
    }

    if (!f->writeable) {
        js_error(J, "File was opened for reading!");
        return;
    } else {
        int ch = js_toint16(J, 1);
        fputc((char)ch, f->file);
        fflush(f->file);
    }
}

/**
 * @brief write a bytes to a file.
 * file.WriteBytes(data:number[])
 *
 * @param J VM state.
 */
static void File_WriteBytes(js_State *J) {
    file_t *f = js_touserdata(J, 0, TAG_FILE);
    if (!f->file) {
        js_error(J, "File was closed!");
        return;
    }

    if (!f->writeable) {
        js_error(J, "File was opened for reading!");
        return;
    } else {
        if (js_isarray(J, 1)) {
            int len = js_getlength(J, 1);

            uint8_t *data = malloc(len);
            if (!data) {
                JS_ENOMEM(J);
                return;
            }

            for (int i = 0; i < len; i++) {
                js_getindex(J, 1, i);
                data[i] = (uint8_t)js_toint16(J, -1);
                js_pop(J, 1);
            }
            fwrite(data, 1, len, f->file);

            free(data);
        } else {
            JS_ENOARR(J);
        }
    }
}

/**
 * @brief write a string (terminated by a NEWLINE) to a file.
 * file.WriteLine(txt:string)
 *
 * @param J VM state.
 */
static void File_WriteLine(js_State *J) {
    file_t *f = js_touserdata(J, 0, TAG_FILE);
    if (!f->file) {
        js_error(J, "File was closed!");
        return;
    }

    const char *line = js_tostring(J, 1);

    if (!f->writeable) {
        js_error(J, "File was opened for reading!");
        return;
    } else {
        fputs(line, f->file);
        fputc('\n', f->file);
        fflush(f->file);
    }
}

/**
 * @brief write a string to a file.
 * file.WriteString(txt:string)
 *
 * @param J VM state.
 */
static void File_WriteString(js_State *J) {
    file_t *f = js_touserdata(J, 0, TAG_FILE);
    if (!f->file) {
        js_error(J, "File was closed!");
        return;
    }

    const char *line = js_tostring(J, 1);

    if (!f->writeable) {
        js_error(J, "File was opened for reading!");
        return;
    } else {
        fputs(line, f->file);
        fflush(f->file);
    }
}

/***********************
** exported functions **
***********************/
/**
 * @brief initialize file subsystem.
 *
 * @param J VM state.
 */
void init_file(js_State *J) {
    DEBUGF("%s\n", __PRETTY_FUNCTION__);

    js_newobject(J);
    {
        NPROTDEF(J, File, ReadByte, 0);
        NPROTDEF(J, File, ReadBytes, 0);
        NPROTDEF(J, File, ReadLine, 0);
        NPROTDEF(J, File, Close, 0);
        NPROTDEF(J, File, WriteByte, 1);
        NPROTDEF(J, File, WriteBytes, 1);
        NPROTDEF(J, File, WriteLine, 1);
        NPROTDEF(J, File, WriteString, 1);
        NPROTDEF(J, File, GetSize, 0);
    }
    CTORDEF(J, new_File, TAG_FILE, 2);

    DEBUGF("%s DONE\n", __PRETTY_FUNCTION__);
}
