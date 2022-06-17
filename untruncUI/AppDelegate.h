//
//  AppDelegate.h
//  untruncUI
//
//  Created by 蒋翌琪 on 2022/6/15.
//

#import <Cocoa/Cocoa.h>
#import <CoreData/CoreData.h>

@interface AppDelegate : NSObject <NSApplicationDelegate>

@property (readonly, strong) NSPersistentCloudKitContainer *persistentContainer;


@end

