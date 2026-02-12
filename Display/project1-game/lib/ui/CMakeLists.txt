SET(SOURCES screens/ui_MainScreen.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/ui_img_img_ellipse_1_png.c
    images/ui_img_img_start_png.c
    images/ui_img_725341252.c
    images/ui_img_img_player1_png.c
    images/ui_img_img_player2_png.c
    images/ui_img_img_player3_png.c
    images/ui_img_img_player4_png.c
    images/ui_img_img_group_winner_png.c
    images/ui_img_2097260803.c
    images/ui_img_1198241503.c
    images/ui_img_img_pressjoyn_png.c
    images/ui_img_img_reactmode_png.c
    images/ui_img_img_shakemode_png.c
    fonts/ui_font_MonserratBold64V2.c
    fonts/ui_font_MontserratBold120.c)

add_library(ui ${SOURCES})
