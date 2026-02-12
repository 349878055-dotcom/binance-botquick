cmake_minimum_required(VERSION 3.10)
project(nowcore CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 3. 查找依赖库
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# 4. 包含头文件
# 添加当前目录以找到 json.hpp
include_directories(
    .
    ${CURL_INCLUDE_DIRS} 
    ${OPENSSL_INCLUDE_DIR}
)

# 5. 源文件管理
add_executable(nowcore main.cpp Network.cpp Common.cpp Strategy.cpp Executor.cpp)

# 6. 链接库
if(WIN32)
    target_link_libraries(nowcore PRIVATE ${CURL_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto ws2_32 Threads::Threads)
else()
    target_link_libraries(nowcore PRIVATE ${CURL_LIBRARIES} OpenSSL::Crypto OpenSSL::SSL Threads::Threads)
endif()
