add_executable(xapp_rc_slice_ctrl
  xapp_rc_slice_ctrl.c
  ../../../../src/util/alg_ds/alg/defer.c
  )

target_link_libraries(xapp_rc_slice_ctrl
                      PUBLIC
                      e42_xapp
                      -pthread
                      -lsctp
                      -ldl
                      )
