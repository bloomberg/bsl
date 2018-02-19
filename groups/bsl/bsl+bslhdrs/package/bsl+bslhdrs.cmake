include(bde_interface_target)
include(bde_package)
include(bde_struct)
include(bde_utils)

function(process_package retPackage listFile uorName)
    bde_assert_no_extra_args()

    get_filename_component(packageName ${listFile} NAME_WE)
    get_filename_component(listDir ${listFile} DIRECTORY)
    get_filename_component(rootDir ${listDir} DIRECTORY)

    # Sources and headers
    bde_utils_add_meta_file("${listDir}/${packageName}.pub" headers TRACK)
    bde_utils_list_template_substitute(headers "%" "${rootDir}/%" ${headers})

    bde_struct_create(
        package
        BDE_PACKAGE_TYPE
        NAME ${packageName}
        HEADERS "${headers}"
    )

    bde_create_package_interfaces(${package} ${listFile})
    bde_setup_package_headers(${package} ${listFile} ${uorName})
    bde_create_package_target(${package})

    bde_return(${package})
endfunction()
