#pragma once
#include <string>

class MediaIpcClient {
public:
    MediaIpcClient(const std::string& sock_path);
    std::string request(const std::string& json_line);
    std::string query_state();
private:
    std::string sock_path_;
};
