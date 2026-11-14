// Native Cocoa popup for the shared FLAC/MP3 player engine.
#define PLAYER_GUI
#include "../main.cpp"

#import <Cocoa/Cocoa.h>
#import <AppKit/AppKit.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <cstdlib>

@interface MusicWindow : NSWindow
@end

@implementation MusicWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (void)keyDown:(NSEvent *)event {
    PlayerKeyView *view = (PlayerKeyView *)self.contentView;
    [view keyDown:event];
}
@end

@interface PlayerController : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) MusicWindow *window;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic, strong) NSMutableArray<NSButton *> *buttons;
@property(nonatomic, strong) NSImageView *coverView;
@property(nonatomic, strong) NSTextField *titleLabel;
@property(nonatomic, strong) NSTextField *artistLabel;
@property(nonatomic, strong) NSTextField *detailsLabel;
@property(nonatomic, strong) NSTextField *songLabel;
@property(nonatomic, strong) NSSlider *slider;
@property(nonatomic, strong) NSView *root;
@property(nonatomic) double phase;
- (void)refresh;
- (void)seekBy:(double)seconds;
- (void)skip;
- (void)back;
- (void)nextFile;
- (void)chooseFolder:(id)sender;
@end

@interface PlayerKeyView : NSView
@property(nonatomic, weak) PlayerController *controller;
@end

namespace {
struct MacState {
    std::vector<std::string> files, titles;
    bool folderMode = false;
    size_t idx = 0;
    std::unique_ptr<Player> p;
    Marks marks;
    std::string title, artist, album, date, status;
    Cover cover;
    ma_uint64 total = 0;
    ma_uint32 rate = 44100;
} app;

ma_uint64 position() { return app.p ? app.p->pos.load() : 0; }
bool paused() { return app.p && app.p->paused.load(); }
void stop_player() {
    if (app.p) { close_player(*app.p); app.p.reset(); }
}
std::string utf8(NSString *s) {
    const char *p = s.fileSystemRepresentation;
    return p ? p : "";
}
NSString *ns(const std::string& s) {
    NSString *v = [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
    return v ?: [NSString stringWithUTF8String:s.c_str()];
}
void set_files(const std::vector<std::string>& paths) {
    app.files.clear(); app.titles.clear(); app.folderMode = false;
    for (const auto& p : paths) {
        if (is_dir(p)) { collect_audio(p, app.files); app.folderMode = true; }
        else app.files.push_back(p);
    }
    for (const auto& f : app.files) {
        const Tags t = read_tags(f);
        auto it = t.find("TITLE");
        app.titles.push_back(it != t.end() && !it->second.empty() ? it->second : display_name(f));
    }
}
}

@implementation PlayerController

- (NSTextField *)label:(NSRect)r size:(CGFloat)size color:(NSColor *)color bold:(BOOL)bold {
    NSTextField *v = [[NSTextField alloc] initWithFrame:r];
    v.editable = NO; v.selectable = NO; v.bezeled = NO; v.drawsBackground = NO;
    v.textColor = color; v.font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
    v.lineBreakMode = NSLineBreakByTruncatingTail;
    [self.root addSubview:v];
    return v;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSRect frame = NSMakeRect(0, 0, 680, 400);
    self.window = [[MusicWindow alloc] initWithContentRect:frame styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable) backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"player"; self.window.center; self.window.releasedWhenClosed = NO;
    self.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    self.window.backgroundColor = [NSColor colorWithCalibratedRed:0.157 green:0.157 blue:0.157 alpha:1];
    self.window.delegate = self;
    PlayerKeyView *keyView = [[PlayerKeyView alloc] initWithFrame:self.window.contentView.bounds];
    keyView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable; keyView.controller = self;
    self.window.contentView = keyView; self.root = keyView;
    self.coverView = [[NSImageView alloc] initWithFrame:NSMakeRect(22, 151, 210, 210)];
    self.coverView.imageScaling = NSImageScaleProportionallyUpOrDown;
    self.coverView.wantsLayer = YES; self.coverView.layer.cornerRadius = 6; self.coverView.layer.masksToBounds = YES;
    [self.root addSubview:self.coverView];

    NSColor *white = [NSColor colorWithCalibratedRed:0.922 green:0.859 blue:0.698 alpha:1];
    NSColor *dim = [NSColor colorWithCalibratedRed:0.659 green:0.600 blue:0.518 alpha:1];
    NSColor *teal = [NSColor colorWithCalibratedRed:0.980 green:0.741 blue:0.184 alpha:1];
    self.titleLabel = [self label:NSMakeRect(252, 330, 406, 38) size:24 color:white bold:YES];
    self.artistLabel = [self label:NSMakeRect(252, 306, 406, 24) size:15 color:teal bold:NO];
    self.detailsLabel = [self label:NSMakeRect(252, 284, 406, 20) size:12 color:dim bold:NO];
    self.songLabel = [self label:NSMakeRect(252, 116, 406, 150) size:13 color:white bold:NO];
    self.songLabel.maximumNumberOfLines = 7;

    self.slider = [[NSSlider alloc] initWithFrame:NSMakeRect(252, 78, 406, 20)];
    self.slider.minValue = 0; self.slider.maxValue = 1; self.slider.target = self; self.slider.action = @selector(progressChanged:);
    [self.root addSubview:self.slider];
    NSTextField *time = [self label:NSMakeRect(252, 59, 80, 16) size:11 color:dim bold:NO]; time.tag = 100;
    NSTextField *duration = [self label:NSMakeRect(578, 59, 80, 16) size:11 color:dim bold:NO]; duration.tag = 101; duration.alignment = NSTextAlignmentRight;

    NSArray *names = @[@"|<", @"▶ / ||", @">|", @"−5s", @"+5s", @"Open…"];
    self.buttons = [NSMutableArray array];
    for (NSUInteger i = 0; i < names.count; ++i) {
        NSButton *b = [NSButton buttonWithTitle:names[i] target:self action:@selector(buttonPressed:)];
        b.tag = (NSInteger)i; b.frame = NSMakeRect(252 + i * 68, 18, 62, 32);
        b.bezelStyle = NSBezelStyleRounded; [self.root addSubview:b]; [self.buttons addObject:b];
    }

    if (app.files.empty()) [self chooseFolder:nil]; else [self load:0];
    [self.window makeKeyAndOrderFront:nil]; [self.window makeFirstResponder:keyView]; [NSApp activateIgnoringOtherApps:YES];
    self.timer = [NSTimer scheduledTimerWithTimeInterval:0.033 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
}

- (void)dealloc { [self.timer invalidate]; stop_player(); }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { (void)sender; return YES; }

- (void)load:(size_t)i {
    stop_player(); app.status.clear();
    if (app.files.empty()) { app.status = "No .mp3 or .flac files found"; [self refresh]; return; }
    for (size_t tries = 0; tries < app.files.size(); ++tries, i = (i + 1) % app.files.size()) {
        app.idx = i; const std::string& f = app.files[i];
        const Tags t = read_tags(f);
        auto get = [&](const char *k) { auto it=t.find(k); return it==t.end()?std::string():it->second; };
        app.title = get("TITLE").empty() ? display_name(f) : get("TITLE");
        app.artist = get("ARTIST").empty() ? get("ALBUMARTIST") : get("ARTIST");
        app.album = get("ALBUM"); app.date = get("DATE"); app.cover = load_cover(f);
        app.marks.clear();
        if (app.folderMode) app.marks.push_back(0);
        else if (!read_stored_marks(f, app.marks)) app.marks = get_marks(f, false);
        app.p.reset(new Player);
        if (open_and_play(*app.p, f)) {
            app.rate = app.p->decoder.outputSampleRate; app.total = 0;
            ma_decoder_get_length_in_pcm_frames(&app.p->decoder, &app.total);
            [self refresh]; return;
        }
        app.p.reset();
    }
    app.status = "None of these files could be played"; app.total = 0; [self refresh];
}

- (void)chooseFolder:(id)sender {
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel]; panel.canChooseFiles = YES; panel.canChooseDirectories = YES;
    panel.allowsMultipleSelection = YES; panel.prompt = @"Open"; panel.message = @"Choose music files or folders";
    if ([panel runModal] != NSModalResponseOK) { if (app.files.empty()) [NSApp terminate:nil]; return; }
    std::vector<std::string> paths; for (NSURL *u in panel.URLs) paths.push_back(utf8(u.path));
    set_files(paths); if (app.files.empty()) app.status = "No .mp3 or .flac files found"; else [self load:0];
    [self refresh];
}

- (void)buttonPressed:(NSButton *)sender {
    switch (sender.tag) {
        case 0: [self back]; break;
        case 1: if (app.p) app.p->paused.store(!paused()); break;
        case 2: [self skip]; break;
        case 3: [self seekBy:-5]; break;
        case 4: [self seekBy:5]; break;
        case 5: [self chooseFolder:nil]; break;
    }
    [self refresh];
}
- (void)progressChanged:(NSSlider *)sender {
    if (app.p && app.total) [self seek:(ma_uint64)(sender.doubleValue * app.total)];
}
- (void)seek:(ma_uint64)frame {
    if (!app.p) return;
    if (app.total && frame >= app.total) { [self nextFile]; return; }
    seek_to(*app.p, frame);
}
- (void)seekBy:(double)sec {
    if (!app.p) return;
    const double target = (double)position() + sec * app.rate;
    [self seek:target <= 0 ? 0 : (ma_uint64)target];
}
- (void)nextFile { if (!app.files.empty()) [self load:(app.idx + 1) % app.files.size()]; }
- (void)prevFile { if (!app.files.empty()) [self load:(app.idx + app.files.size() - 1) % app.files.size()]; }
- (void)skip {
    if (!app.p || app.files.empty()) return;
    if (app.folderMode) { [self nextFile]; return; }
    auto it = std::upper_bound(app.marks.begin(), app.marks.end(), position());
    if (it == app.marks.end()) [self nextFile]; else [self seek:*it];
}
- (void)back {
    if (!app.p || app.files.empty()) return;
    const ma_uint64 grace = (ma_uint64)(app.rate * BACK_GRACE_SEC);
    if (app.folderMode) { if (position() < grace) [self prevFile]; else [self seek:0]; return; }
    const size_t cur = current_song(app.marks, position());
    [self seek:app.marks[position() < app.marks[cur] + grace && cur > 0 ? cur - 1 : cur]];
}

- (void)tick:(NSTimer *)timer {
    (void)timer;
    if (app.p && app.p->finished.load()) [self nextFile];
    self.phase += paused() ? 0 : 0.3;
    [self refresh];
}
- (void)refresh {
    NSColor *teal = [NSColor colorWithCalibratedRed:0.980 green:0.741 blue:0.184 alpha:1];
    self.titleLabel.stringValue = ns(app.title);
    self.artistLabel.stringValue = ns(app.artist.empty() ? "Unknown artist" : app.artist);
    self.artistLabel.textColor = teal;
    self.detailsLabel.stringValue = ns(app.album + (app.date.empty()?"":"  ·  "+app.date) + "  ·  " + (app.files.empty()?"0":std::to_string(app.idx+1)+"/"+std::to_string(app.files.size())));
    if (app.cover.ok() && g_cfg.cover) {
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:app.cover.w pixelsHigh:app.cover.h bitsPerSample:8 samplesPerPixel:3 hasAlpha:NO isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:app.cover.w*3 bitsPerPixel:24];
        memcpy(rep.bitmapData, app.cover.rgb.data(), app.cover.rgb.size());
        self.coverView.image = [[NSImage alloc] initWithSize:NSMakeSize(app.cover.w,app.cover.h)]; [self.coverView.image addRepresentation:rep];
    } else self.coverView.image = nil;
    const ma_uint64 p = position();
    self.slider.doubleValue = app.total ? std::min(1.0, (double)p/app.total) : 0;
    for (NSTextField *f in self.root.subviews) if ([f isKindOfClass:NSTextField.class] && (f.tag==100 || f.tag==101))
        f.stringValue = ns(mmss(f.tag==100?p:app.total, app.rate));
    NSMutableString *list = [NSMutableString string];
    if (!app.status.empty()) [list appendString:ns(app.status)];
    else {
        const size_t count = app.folderMode ? app.files.size() : app.marks.size();
        const size_t cur = app.folderMode ? app.idx : current_song(app.marks,p);
        const size_t first = count > 7 ? std::min(cur > 3 ? cur-3 : 0, count-7) : 0;
        for (size_t j=first; j<count && j<first+7; ++j) {
            if (j>first) [list appendString:@"\n"];
            if (j==cur) [list appendString:@"▸ "]; else [list appendString:@"  "];
            [list appendString:app.folderMode ? ns(app.titles[j]) : [NSString stringWithFormat:@"Song %zu   %s",j+1,mmss(app.marks[j],app.rate).c_str()]];
        }
    }
    self.songLabel.stringValue = list;
    self.window.title = [NSString stringWithFormat:@"player - %@%@%@", paused()?@"(paused) ":@"", ns(app.title), app.artist.empty()?@"": [@" - " stringByAppendingString:ns(app.artist)]];
    (void)teal;
}

- (BOOL)windowShouldClose:(id)sender { (void)sender; stop_player(); return YES; }
@end

@implementation PlayerKeyView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)event {
    NSString *s = event.charactersIgnoringModifiers.lowercaseString;
    unichar c = s.length ? [s characterAtIndex:0] : 0;
    PlayerController *p = self.controller;
    switch (c) {
        case ' ': if (app.p) app.p->paused.store(!paused()); break;
        case NSLeftArrowFunctionKey: case 'h': [p seekBy:-5]; break;
        case NSRightArrowFunctionKey: case 'l': [p seekBy:5]; break;
        case 's': [p skip]; break;
        case 'b': [p back]; break;
        case 'n': [p nextFile]; break;
        case 'o': [p chooseFolder:nil]; break;
        case 'q': case 27: [NSApp terminate:nil]; break;
        default: [super keyDown:event]; return;
    }
    [p refresh];
}
@end

static std::string executable_dir() {
    uint32_t n = 0; _NSGetExecutablePath(nullptr, &n);
    std::vector<char> b(n + 1); if (_NSGetExecutablePath(b.data(), &n) != 0) return ".";
    char resolved[PATH_MAX]; if (!realpath(b.data(), resolved)) return ".";
    std::string path(resolved); const size_t slash=path.find_last_of('/');
    return slash==std::string::npos ? "." : path.substr(0,slash);
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        const std::string exeDir = executable_dir(); std::string configPath;
        std::vector<std::string> paths;
        for (int i=1; i<argc; ++i) {
            if (std::strcmp(argv[i],"--config")==0 && i+1<argc) configPath=argv[++i];
            else if (argv[i][0]!='-') paths.emplace_back(argv[i]);
        }
        if (!configPath.empty()) load_config(configPath);
        else {
            std::vector<std::string> candidates;
            if (const char *env=std::getenv("PLAYER_CONFIG")) candidates.push_back(env);
            candidates.push_back("player.conf"); candidates.push_back(exeDir+"/player.conf");
            if (const char *home=std::getenv("HOME")) candidates.push_back(std::string(home)+"/.config/player/config");
            for (const auto& p:candidates) if (load_config(p)) break;
        }
        set_files(paths);
        [NSApplication sharedApplication];
        PlayerController *delegate = [PlayerController new];
        NSApp.delegate = delegate;
        [NSApp run];
    }
    return 0;
}
