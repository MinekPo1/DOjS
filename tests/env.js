/*
** This function is called once when the script is started.
*/
function Setup() {
    SetFramerate(30);
}

/*
** This function is repeatedly until ESC is pressed or Stop() is called.
*/
function Loop() {
    ClearScreen(EGA.BLACK);
    TextXY(SizeX() / 2, SizeY() / 2, "BLASTER="+GetEnv("BLASTER"), EGA.WHITE);
    TextXY(SizeX() / 2, SizeY() / 2+8, "blaster="+GetEnv("blaster"), EGA.WHITE);
}

/*
** This function is called on any input.
*/
function Input(event) {
}
