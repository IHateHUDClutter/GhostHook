#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define SH_MENU_OVERLAY_ROWS 12

typedef struct {
    char name[48];
    char value[32];
    int shown;
    int selected;
} ShMenuOverlayRow;

typedef struct {
    char title[48];
    char hint[384];
    char status[96];
    char footer[32];
    int rows;
    int sel;
    int isRoot;
    ShMenuOverlayRow row[SH_MENU_OVERLAY_ROWS];
} ShMenuOverlayView;

void ShMenuOverlaySetReady(int ready);
int ShMenuOverlayCapture(ShMenuOverlayView *view);
float ShMenuOverlayScale(void);
int ShMenuOverlayFontChoice(void);
int ShMenuOverlayFontSize(void);
unsigned ShMenuOverlayTextColour(void);
unsigned ShMenuOverlayBackgroundAlpha(void);
unsigned ShMenuOverlayAccentColour(void);
void ShOverlayStart(void);

#ifdef __cplusplus
}
#endif
