#include <kernel/drivers/audio/song.h>  // for Note, Song

// Ode to Joy melody transcription
static Note odeToJoy[] = {
    {329, 300}, // E4
    {329, 300}, // E4
    {349, 300}, // F4
    {392, 300}, // G4
    {392, 300}, // G4
    {349, 300}, // F4
    {329, 300}, // E4
    {293, 300}, // D4
    {261, 300}, // C4
    {261, 300}, // C4
    {293, 300}, // D4
    {329, 300}, // E4
    {329, 300}, // E4
    {293, 300}, // D4
    {293, 300}, // D4
    {329, 300}, // E4
    {329, 300}, // E4
    {349, 300}, // F4
    {392, 300}, // G4
    {392, 300}, // G4
    {349, 300}, // F4
    {329, 300}, // E4
    {293, 300}, // D4
    {261, 300}, // C4
    {261, 300}, // C4
    {293, 300}, // D4
    {329, 300}, // E4
    {293, 300}, // D4
    {261, 300}, // C4
    {261, 300}  // C4
};

Song testSong = {
    .notes  = odeToJoy,
    .length = sizeof(odeToJoy) / sizeof(Note)
};
