//
//  untrunc_main.hpp
//  untruncUI
//
//  Created by 蒋翌琪 on 2022/6/15.
//

#ifndef untrunc_main_hpp
#define untrunc_main_hpp

#include "mp4.h"
#include "atom.h"
#include "log.h"
#include "avlog.h"
#include "track.h"
#include "file.h"
#include "codec.h"
#include "codecstats.h"

#include <cstdlib>
#include <iostream>
#include <string>


#endif /* untrunc_main_hpp */

void usage();
void searchFile(std::string ok, std::vector<uint8_t> search);
int toHex(char s);
int main_API(int argc, char *argv[]);
