include_directories(${PROJECT_SOURCE_DIR} ${HUSKY_EXTERNAL_INCLUDE})

add_executable(BasicExample basic_example.cpp)
target_link_libraries(BasicExample minips)
target_link_libraries(BasicExample ${HUSKY_EXTERNAL_LIB})
set_property(TARGET BasicExample PROPERTY CXX_STANDARD 11)
add_dependencies(BasicExample ${external_project_dependencies})