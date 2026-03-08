# TCP echo server example
add_executable(tcp_echo_server tcp_echo_server.cpp)
target_link_libraries(tcp_echo_server PRIVATE socketpp)
socketpp_configure_executable(tcp_echo_server)

# TCP echo client example
add_executable(tcp_echo_client tcp_echo_client.cpp)
target_link_libraries(tcp_echo_client PRIVATE socketpp)
socketpp_configure_executable(tcp_echo_client)

# UDP echo server example
add_executable(udp_echo_server udp_echo_server.cpp)
target_link_libraries(udp_echo_server PRIVATE socketpp)
socketpp_configure_executable(udp_echo_server)

# UDP batch recv example
add_executable(udp_batch_recv udp_batch_recv.cpp)
target_link_libraries(udp_batch_recv PRIVATE socketpp)
socketpp_configure_executable(udp_batch_recv)

# UDP DNS query client example
add_executable(udp_dns_query udp_dns_query.cpp)
target_link_libraries(udp_dns_query PRIVATE socketpp)
socketpp_configure_executable(udp_dns_query)
