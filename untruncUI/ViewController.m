//
//  ViewController.m
//  untruncUI
//
//  Created by 蒋翌琪 on 2022/6/15.
//

#import "ViewController.h"
#include "untrunc_main.hpp"

@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];

    // Do any additional setup after loading the view.
}


- (void)setRepresentedObject:(id)representedObject {
    [super setRepresentedObject:representedObject];

    // Update the view, if already loaded.
}



- (IBAction)Select_ref:(id)sender {
    if(Ref_path!=NULL){
        free(Ref_path);
        Ref_path=NULL;
    }
    NSOpenPanel* panel=[NSOpenPanel new];
    [panel setCanChooseFiles:true];
    [panel setCanChooseDirectories:false];
    [panel setAllowedFileTypes:[NSArray arrayWithObject:@"mp4"]];
    [panel setAllowsMultipleSelection:false];
    [panel setAllowsOtherFileTypes:false];
    if(NSModalResponseOK != [panel runModal])
        return;
    const char* temp=[panel URL].path.UTF8String;
    Ref_path=(char*)malloc(strlen(temp)+1);
    strcpy(Ref_path, temp);
    return;
}

- (IBAction)Select_rep:(id)sender {
    if(Rep_path!=NULL){
        free(Rep_path);
        Rep_path=NULL;
    }
    NSOpenPanel* panel=[NSOpenPanel new];
    [panel setCanChooseFiles:true];
    [panel setCanChooseDirectories:false];
    [panel setAllowedFileTypes:[NSArray arrayWithObject:@"mp4"]];
    [panel setAllowsMultipleSelection:false];
    [panel setAllowsOtherFileTypes:false];
    if(NSModalResponseOK != [panel runModal])
        return;
    const char* temp=[panel URL].path.UTF8String;
    Rep_path=(char*)malloc(strlen(temp)+1);
    strcpy(Rep_path, temp);
    return;
}


- (IBAction)Btn_Submit:(id)sender {
    NSAlert* alert=[NSAlert new];
    alert.alertStyle=NSAlertStyleWarning;
    [alert addButtonWithTitle:@"确定"];
    
    if(Ref_path!=NULL&&Rep_path!=NULL){
        NSSavePanel* panel=[NSSavePanel new];
        [panel setCanCreateDirectories:true];
        [panel setAllowedFileTypes:[NSArray arrayWithObject:@"mp4"]];
        [panel setAllowsOtherFileTypes:false];
        if(!(NSModalResponseOK == [panel runModal]))
            return;
        const char* save_path = [[panel URL].path UTF8String];
        
        /*
         const char* Command[4];
         Command[0]=Ref_path;
         Command[1]=Rep_path;
         Command[2]="-o";
         Command[3]=save_path;
         main_API(4, (char**)Command);
         }
         return;
         */
        
        using namespace std;
        
        std::string output_filename;
        bool info = false;
        bool analyze = false;
        bool simulate = false;
        int analyze_track = -1;
        bool drifting = false;
        Mp4::MdatStrategy mdat_strategy = Mp4::FIRST;
        //bool same_mdat_start = false; //if mdat can be found or starting of packets try using the same absolute offset.
        //bool ignore_mdat_start = false; //ignore mdat string and look for first recognizable packet.
        bool skip_zeros = true;
        int64_t mdat_begin = -1; //start of packets if specified.
        
        output_filename=std::string(save_path);
        
        std::string ok = Ref_path;
        std::string corrupt = Rep_path;
        free(Ref_path);Ref_path=NULL;
        free(Rep_path);Rep_path=NULL;
        
        Mp4 mp4;
        
        try {
            mp4.open(ok);
            
            if(info) {
                mp4.printMediaInfo();
                mp4.printAtoms();
            }
            if(analyze) {
                mp4.analyze(analyze_track);
            }
            if(simulate)
                mp4.simulate(mdat_strategy, mdat_begin);
            
            if(corrupt.size()) {
                
                bool success = mp4.repair(corrupt, mdat_strategy, mdat_begin, skip_zeros, drifting);
                //if the user didn't specify the strategy, try them all.
                if(!success  && mdat_strategy == Mp4::FIRST) {
                    vector<Mp4::MdatStrategy> strategies = { Mp4::SAME, Mp4::SEARCH, Mp4::LAST };
                    for(Mp4::MdatStrategy strategy: strategies) {
                        Log::info << "\n\nTrying a different approach to locate mdat start" << endl;
                        success = mp4.repair(corrupt, strategy, mdat_begin, skip_zeros);
                        if(success) break;
                    }
                }
                if(!success) {
                    alert.messageText=@"错误";
                    alert.informativeText=@"修复失败";
                }
                
                
                mp4.saveVideo(output_filename);
            }
        } catch(string e) {
            Log::error << e << endl;
        } catch(const char *e) {
            Log::error << e << endl;
        }
    }
    
    else {
        alert.messageText = @"没有选择文件";
        alert.informativeText = @"请先选择文件";
        [alert runModal];
    }
}



@end
