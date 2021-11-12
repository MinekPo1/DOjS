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

function Setup() {
    MouseShowCursor(false);

    //FlicOpen("tests/EMCONJ.FLC");
    //FlicOpen("tests/EMCONJ1.FLI");
    FlicOpen("tests/GASPRA.FLI");

    Println(FLIC_WIDTH);
    Println(FLIC_HEIGHT);

    SetFramerate(30);
}

function Loop() {
    //  ClearScreen(EGA.DARK_GRAY);
    Println(FlicPlay(100, 100, true));
    Box(100, 100, 100 + FLIC_WIDTH, 100 + FLIC_HEIGHT, EGA.RED);
}

function Input(e) {
}
