# esp_audio_codec 2.5.x emits a raw "-L <path>" link-library argument.
# Windows paths with spaces split that argument at the final link. Convert it
# to a CMake link-directory property so CMake quotes it correctly. This modifies
# target properties only; no generated or managed component files are edited.
idf_component_get_property(pocket_codec_target espressif__esp_audio_codec COMPONENT_LIB)
idf_component_get_property(pocket_codec_dir espressif__esp_audio_codec COMPONENT_DIR)
if(TARGET "${pocket_codec_target}")
    foreach(pocket_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(pocket_original "${pocket_codec_target}" "${pocket_property}")
        if(pocket_original)
            set(pocket_corrected "")
            foreach(pocket_item IN LISTS pocket_original)
                if(NOT pocket_item MATCHES "^-L " AND
                   NOT pocket_item MATCHES "^\\$<LINK_ONLY:-L ")
                    list(APPEND pocket_corrected "${pocket_item}")
                endif()
            endforeach()
            set_target_properties("${pocket_codec_target}" PROPERTIES
                                  "${pocket_property}" "${pocket_corrected}")
        endif()
    endforeach()
    target_link_directories("${pocket_codec_target}" INTERFACE "${pocket_codec_dir}/lib/${IDF_TARGET}")
endif()
