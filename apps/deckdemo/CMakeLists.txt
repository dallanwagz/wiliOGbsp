# The DECK framework demo: every screen kind fwog_ui has, in one app. The
# living documentation of docs/deck-ui-spec.md's developer story.
add_executable(deckdemo_display display/main.c)
target_link_libraries(deckdemo_display fwog_display_bsp)
fwog_display_app(deckdemo_display
    VERSION 001
    DESCRIPTION "DECK UI demo: list, form, story, progress and canvas under the fixed five-color grammar")

add_executable(deckdemo_main main/main.c)
target_link_libraries(deckdemo_main fwog_main_bsp)
fwog_embed_display_image(deckdemo_main)
fwog_main_app(deckdemo_main
    VERSION 001
    DESCRIPTION "DECK UI demo main: brings the display up and idles")
