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

LoadLibrary("qoi");
LoadLibrary("png");
LoadLibrary("jpeg");

/*
** This function is called once when the script is started.
*/
function Setup() {
	SetFramerate(30);
	MouseShowCursor(false);

	i1 = new Bitmap("examples/dojs.bmp");
	i2 = new Bitmap("examples/3dfx.tga");
	i3 = new Bitmap("examples/glow.pcx");
	i6 = new Bitmap("examples/3dfx.png");
	i7 = new Bitmap("examples/3dfx_s.jpg");
	i8 = new Bitmap("examples/3dfx.jpg");
	i9 = new Bitmap("examples/3dfx_bw.jpg");

	i10 = new Bitmap("LEHLh[WB2yk8pyoJadR*.7kCMdnj", 320, 240, 1);
	// i10 = new Bitmap("L", 320, 240, 1);

	// var dat = [];
	// for (var x = 0; x < 255; x++) {
	// 	for (var y = 0; y < 255; y++) {
	// 		dat.push(0xFF000000 | (x << 8) | y);
	// 	}
	// }

	// i4 = new Bitmap(dat, 255, 255);

	var dat = [];
	for (var y = 0; y < 32; y++) {
		for (var x = 0; x < 256; x++) {
			dat.push(0x0000FF | ((y * 8) << 8) | x << 24);
		}
	}

	i4 = new Bitmap(dat, 256, 32);

	img = i10;
	cnt = 0;
}

/*
** This function is repeatedly until ESC is pressed or Stop() is called.
*/
function Loop() {
	ClearScreen(EGA.BLACK);

	if (img != null) {
		img.Draw(0, 0);
	}

	var dat = [];
	for (var x = 0; x < 32; x++) {
		for (var y = 0; y < 32; y++) {
			dat.push(0xFF000000 | ((cnt % 32) * 7 << 16) | (x * 7 << 8) | y * 7);
		}
	}
	DrawArray(dat, SizeX() / 2, SizeY() / 2, 32, 32);
	cnt++;
}

/*
** This function is called on any input.
*/
function Input(e) {
	Println(JSON.stringify(e));
	if (CompareKey(e.key, '1')) {
		img = i1;
		Println(img.constructor.toString());
	}
	if (CompareKey(e.key, '2')) {
		img = i2;
		Println(img.constructor.toString());
	}
	if (CompareKey(e.key, '3')) {
		img = i3;
		Println(img.constructor.toString());
	}
	if (CompareKey(e.key, '4')) {
		img = i4;
		Println(img.constructor.toString());
	}
	if (CompareKey(e.key, '5')) {
		img = new Bitmap(100, 100, 255, 255);
		Println(img.constructor.toString());
		img.SaveTgaImage("5.tga");
		img.SavePcxImage("5.pcx");
		img.SaveBmpImage("5.bmp");
		img.SavePngImage("5.png");
		img.SavePngImage("5.qoi");

		SaveTgaImage("scr.tga");
		SavePcxImage("scr.pcx");
		SaveBmpImage("scr.bmp");
		SavePngImage("scr.qoi");
	}
	if (CompareKey(e.key, '6')) {
		img = i6;
		Println(img.constructor.toString());
	}
	if (CompareKey(e.key, '7')) {
		img = i7;
		Println(img.constructor.toString());
	}
	if (CompareKey(e.key, '8')) {
		img = i8;
		Println(img.constructor.toString());
	}
	if (CompareKey(e.key, '9')) {
		img = i9;
		Println(img.constructor.toString());
	}
	if (CompareKey(e.key, '0')) {
		img = i10;
		Println(img.constructor.toString());
	}
}
