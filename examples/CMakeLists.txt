# The programs of docs/hello-world.md and docs/tutorial.md.
foreach (example hello-world tutorial-writer tutorial-reader)
    add_executable(${example} ${example}.c)
    target_link_libraries(${example} PRIVATE mxl-monitoring-http)
endforeach ()
