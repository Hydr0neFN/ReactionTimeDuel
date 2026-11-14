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
    images/ui_img_img_pressjoyn_png.c
    images/ui_img_img_reactmode_png.c
    images/ui_img_img_shakemode_png.c
    images/ui_img_img_deucemode_png.c
    images/ui_img_hanze_logo.c
    images/ui_img_icon_link_on.c
    images/ui_img_icon_link_off.c
    images/ui_img_icon_gear.c
    fonts/ui_font_MonserratBold64V2.c
    fonts/ui_font_MontserratBold120.c
    fonts/ui_font_MontserratBold20.c
    fonts/ui_font_MontserratBold28.c
    fonts/ui_font_MontserratBold40.c)

add_library(ui ${SOURCES})
