//
//  CurrentTrackOSX.m
//  butt
//
//  Created by Melchor Garau Madrigal on 8/7/15.
//  Copyright (c) 2015 Daniel Nöthen. All rights reserved.
//

#import "CurrentTrackOSX.h"
#include <string.h>
#import <iTunes.h>
#import <Spotify.h>
#import <VOX.h>

const char* getCurrentTrackFromiTunes() {
    char* ret = NULL;
    iTunesApplication *iTunes = [SBApplication applicationWithBundleIdentifier:@"com.apple.itunes"];
    if([iTunes isRunning]) {
        if([iTunes playerState] != iTunesEPlSStopped) {
            NSString* track = [NSString stringWithFormat:@"%@ - %@", [[iTunes currentTrack] name], [[iTunes currentTrack] artist]];
            const char* utf8 = [track UTF8String];
            if(utf8 != NULL) ret = strdup(utf8);
        }
    }
    
    return ret;
}

const char* getCurrentTrackFromSpotify() {
    char* ret = NULL;
    SpotifyApplication *spotify = [SBApplication applicationWithBundleIdentifier:@"com.spotify.client"];
    if([spotify isRunning]) {
        if([spotify playerState] != SpotifyEPlSStopped) {
            NSString* track = [NSString stringWithFormat:@"%@ - %@", [[spotify currentTrack] name], [[spotify currentTrack] artist]];
            const char* utf8 = [track UTF8String];
            if(utf8 != NULL) ret = strdup(utf8);
        }
    }
    
    return ret;
}

const char* getCurrentTrackFromVOX() {
    char* ret = NULL;
    VOXApplication *vox = [SBApplication applicationWithBundleIdentifier:@"com.coppertino.Vox"];
    if([vox isRunning]) {
        if([vox playerState]) {
            NSString* track = [NSString stringWithFormat:@"%@ - %@", [vox track], [vox artist]];
            const char* utf8 = [track UTF8String];
            if(utf8 != NULL) ret = strdup(utf8);
        }
    }
    
    return ret;
}

typedef const char* (*currentTrackFunction)(void);
currentTrackFunction getCurrentTrackFunctionFromId(int i) {
    switch(i) {
        case 0: return &getCurrentTrackFromiTunes;
        case 1: return &getCurrentTrackFromSpotify;
        case 2: return &getCurrentTrackFromVOX;
        default: return NULL;
    }
}