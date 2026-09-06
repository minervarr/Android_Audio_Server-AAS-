# ── aoas_stage_shaders(<target> <out_dir>) ───────────────────────────────────
#
# Puts vk_canvas's compiled shaders where its AssetReader looks for them:
# <exe dir>/assets/shaders on the desktop, the APK's assets/shaders on Android.
#
# COMPILED from .slang, never copied from the .spv files the submodules commit.
# Lifted from navaLauncher's cmake/NavaAssets.cmake, including the reason:
# the committed msdf_vert.spv predates the atlas-page attribute vk_canvas's
# text emitter now writes, and binding it renders every glyph as a solid white
# block.
function(aoas_stage_shaders TARGET OUT_DIR)
    set(_vkc  ${VK_CANVAS_DIR})
    set(_font ${_vkc}/first_party/vulkan_font_engine)

    include(${_vkc}/cmake/VceShaders.cmake)

    vce_compile_slang(aoas_shaders_font ${OUT_DIR}/shaders ${_font}/shaders_src
        composite_vert composite_frag tiling coverage msdf_vert msdf_frag)

    vce_compile_slang(aoas_shaders_canvas ${OUT_DIR}/shaders ${_vkc}/shaders_src
        overlay_vert overlay_frag image_vert image_frag shape_vert shape_frag)

    add_dependencies(${TARGET} aoas_shaders_font aoas_shaders_canvas)
endfunction()

# ── aoas_stage_font(<target> <out_dir>) ──────────────────────────────────────
#
# The DESKTOP half. Android does not use it: assets/ is an extra
# assets.srcDir in app/build.gradle instead, because AGP merges assets BEFORE
# it runs the native build, so a POST_BUILD copy would always be one build too
# late to be packaged.
function(aoas_stage_font TARGET OUT_DIR)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OUT_DIR}/fonts
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${AOAS_ROOT}/assets/fonts/ui/ui.otf ${OUT_DIR}/fonts/ui.otf
        COMMENT "Staging font into ${OUT_DIR}")
endfunction()
