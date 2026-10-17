// Case-insensitive loader over a directory of files extracted from CASC.
// OpenBW asks for names such as "arr/units.dat" or "Tileset/Jungle.cv5"; CASC names use mixed case.
#pragma once
#include "bwgame.h"
#include <filesystem>
#include <fstream>
#include <unordered_map>
struct dir_loader {
	std::unordered_map<std::string, std::filesystem::path> idx;
	static std::string norm(std::string s){ for(auto&c:s){ c=(char)tolower((unsigned char)c); if(c=='\\') c='/'; } return s; }
	explicit dir_loader(const std::filesystem::path& root){
		for(auto& e: std::filesystem::recursive_directory_iterator(root))
			if(e.is_regular_file()) idx[norm(std::filesystem::relative(e.path(),root).generic_string())] = e.path();
	}
	void operator()(bwgame::a_vector<uint8_t>& dst, bwgame::a_string name){
		auto it = idx.find(norm(name));
		if(it==idx.end()) bwgame::error("dir_loader: %s: file not found", name);
		std::ifstream f(it->second, std::ios::binary);
		dst.assign(std::istreambuf_iterator<char>(f), {});
	}
};
