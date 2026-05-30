#import "ViewController.h"
#import "GameView.h"
#import "NullspaceIOS.h"
#import <QuartzCore/CAEAGLLayer.h>

@interface ViewController ()
@property (nonatomic, strong) GameView*      gameView;
@property (nonatomic, strong) UIView*        menuView;
@property (nonatomic, strong) UILabel*       usernameLabel;
@property (nonatomic, strong) CADisplayLink* displayLink;
@property (nonatomic)         BOOL           gameInitialized;
@property (nonatomic)         BOOL           requestedLandscapeGeometry;
@end

@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];
    [self setupGameView];
    [self setupMenuView];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];

    CGRect bounds = self.view.bounds;
    self.gameView.frame = bounds;
    self.menuView.frame = bounds;

    float scale = UIScreen.mainScreen.nativeScale;
    self.gameView.contentScaleFactor = scale;
    ((CAEAGLLayer*)self.gameView.layer).contentsScale = scale;

    if (!self.gameInitialized &&
        bounds.size.width > 0.0 &&
        bounds.size.height > 0.0 &&
        bounds.size.width > bounds.size.height) {
        [self initGame];
    }
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self requestLandscapeGeometryIfNeeded];
}

- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }

- (void)requestLandscapeGeometryIfNeeded {
    if (self.requestedLandscapeGeometry) return;
    self.requestedLandscapeGeometry = YES;

    if (@available(iOS 16.0, *)) {
        UIWindowScene* scene = self.view.window.windowScene;
        if (scene) {
            UIWindowSceneGeometryPreferencesIOS* prefs =
                [[UIWindowSceneGeometryPreferencesIOS alloc]
                    initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscapeRight];
            [scene requestGeometryUpdateWithPreferences:prefs errorHandler:^(NSError* error) {
                NSLog(@"[nullspace] landscape geometry request failed: %@", error);
            }];
            [self setNeedsUpdateOfSupportedInterfaceOrientations];
        }
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [[UIDevice currentDevice] setValue:@(UIInterfaceOrientationLandscapeRight) forKey:@"orientation"];
#pragma clang diagnostic pop
        [UIViewController attemptRotationToDeviceOrientation];
    }
}

// ── OpenGL ES game view ──────────────────────────────────────────────────────

- (void)setupGameView {
    self.gameView = [[GameView alloc] initWithFrame:self.view.bounds];
    self.gameView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.gameView.multipleTouchEnabled = YES;
    self.gameView.contentScaleFactor = UIScreen.mainScreen.nativeScale;

    CAEAGLLayer* eaglLayer = (CAEAGLLayer*)self.gameView.layer;
    eaglLayer.opaque = YES;
    eaglLayer.contentsScale = UIScreen.mainScreen.nativeScale;
    eaglLayer.drawableProperties = @{
        kEAGLDrawablePropertyRetainedBacking: @NO,
        kEAGLDrawablePropertyColorFormat:     kEAGLColorFormatRGBA8
    };

    [self.view addSubview:self.gameView];
}

- (void)initGame {
    self.gameInitialized = YES;

    float  scale  = UIScreen.mainScreen.nativeScale;

    // Use the actual view bounds (in points) — this reflects the real
    // orientation/window size at runtime.
    CGRect b = self.view.bounds;
    int physW = (int)(b.size.width  * scale);
    int physH = (int)(b.size.height * scale);

    self.gameView.frame = b;

    CAEAGLLayer* layer = (CAEAGLLayer*)self.gameView.layer;
    layer.contentsScale = scale;
    // Don't override layer.bounds — let it match the view (gameView.frame above).

    bool isTablet = (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad);
    iOSInit((__bridge void*)layer, physW, physH, scale, isTablet);

    [self updateUsernameLabel];
}

// ── UIKit main menu ──────────────────────────────────────────────────────────

- (void)setupMenuView {
    CGFloat w = self.view.bounds.size.width;
    CGFloat h = self.view.bounds.size.height;

    UIView* menu = [[UIView alloc] initWithFrame:self.view.bounds];
    menu.backgroundColor   = [UIColor blackColor];
    menu.autoresizingMask  = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.menuView = menu;
    [self.view addSubview:menu];

    // Title
    UILabel* title = [[UILabel alloc] initWithFrame:CGRectMake(0, h * 0.08, w, 70)];
    title.text          = @"NULLORBIT";
    title.textColor     = [UIColor colorWithRed:0.71 green:0.71 blue:1.0 alpha:1.0];
    title.font          = [UIFont boldSystemFontOfSize:52];
    title.textAlignment = NSTextAlignmentCenter;
    [menu addSubview:title];

    // Username
    self.usernameLabel = [[UILabel alloc] initWithFrame:CGRectMake(20, h * 0.28, w - 120, 28)];
    self.usernameLabel.textColor     = [UIColor colorWithRed:0.45 green:1.0 blue:0.39 alpha:1.0];
    self.usernameLabel.font          = [UIFont systemFontOfSize:17];
    self.usernameLabel.textAlignment = NSTextAlignmentCenter;
    [menu addSubview:self.usernameLabel];

    // Reset button
    UIButton* reset = [UIButton buttonWithType:UIButtonTypeSystem];
    reset.frame = CGRectMake(w - 100, h * 0.28 - 4, 80, 36);
    [reset setTitle:@"RESET" forState:UIControlStateNormal];
    [reset setTitleColor:[UIColor colorWithRed:0.87 green:0.19 blue:0.03 alpha:1.0]
                forState:UIControlStateNormal];
    reset.layer.borderColor  = [UIColor darkGrayColor].CGColor;
    reset.layer.borderWidth  = 1;
    reset.layer.cornerRadius = 6;
    [reset addTarget:self action:@selector(onReset) forControlEvents:UIControlEventTouchUpInside];
    [menu addSubview:reset];

    // Trench Wars CTF
    UIButton* tw = [self makeMenuButton:@"Trench Wars: Capture the Flag"
                                  frame:CGRectMake(20, h * 0.45, w - 40, 70)];
    [tw addTarget:self action:@selector(onJoinTW) forControlEvents:UIControlEventTouchUpInside];
    [menu addSubview:tw];

    // Team Deathmatch
    UIButton* tdm = [self makeMenuButton:@"Team Deathmatch"
                                   frame:CGRectMake(20, h * 0.45 + 90, w - 40, 70)];
    [tdm addTarget:self action:@selector(onJoinTDM) forControlEvents:UIControlEventTouchUpInside];
    [menu addSubview:tdm];
}

- (UIButton*)makeMenuButton:(NSString*)title frame:(CGRect)frame {
    UIButton* btn = [UIButton buttonWithType:UIButtonTypeSystem];
    btn.frame = frame;
    [btn setTitle:title forState:UIControlStateNormal];
    [btn setTitleColor:[UIColor colorWithRed:0.937 green:0.678 blue:0.129 alpha:1.0]
              forState:UIControlStateNormal];
    btn.titleLabel.font = [UIFont boldSystemFontOfSize:18];
    btn.backgroundColor = [UIColor colorWithWhite:0.14 alpha:1.0];
    btn.layer.cornerRadius = 8;
    return btn;
}

- (void)updateUsernameLabel {
    const char* name = iOSGetUsername();
    if (name && name[0]) {
        self.usernameLabel.text = [NSString stringWithFormat:@"Player: %s", name];
    }
}

- (void)onReset {
    iOSRegenerateUsername();
    [self updateUsernameLabel];
}

// server index 1 = Null Orbit production server (same as Android)
- (void)onJoinTW  { [self startGame:1 arena:"pub"]; }
- (void)onJoinTDM { [self startGame:1 arena:"tdm"]; }

// ── Game loop ────────────────────────────────────────────────────────────────

- (void)startGame:(int)serverIndex arena:(const char*)arena {
    self.menuView.hidden = YES;

    iOSJoinZone(serverIndex, arena);

    self.displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(gameLoop)];
    [self.displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)gameLoop {
    iOSTick();

    if (!iOSIsInGame()) {
        [self.displayLink invalidate];
        self.displayLink = nil;
        self.menuView.hidden = NO;
        [self updateUsernameLabel];
    }
}

// ── Orientation ──────────────────────────────────────────────────────────────

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscapeLeft | UIInterfaceOrientationMaskLandscapeRight;
}

- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation {
    return UIInterfaceOrientationLandscapeRight;
}

- (BOOL)prefersStatusBarHidden { return YES; }

// ── Touch forwarding ─────────────────────────────────────────────────────────

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!self.menuView.hidden) return;
    float scale = UIScreen.mainScreen.nativeScale;
    int sw = (int)(self.view.bounds.size.width * scale);
    int sh = (int)(self.view.bounds.size.height * scale);
    for (UITouch* t in touches) {
        CGPoint p = [t locationInView:self.view];
        iOSTouchBegan(p.x * scale, p.y * scale, (long)(intptr_t)t, sw, sh);
    }
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!self.menuView.hidden) return;
    float scale = UIScreen.mainScreen.nativeScale;
    int sw = (int)(self.view.bounds.size.width * scale);
    int sh = (int)(self.view.bounds.size.height * scale);
    for (UITouch* t in touches) {
        CGPoint p = [t locationInView:self.view];
        iOSTouchMoved(p.x * scale, p.y * scale, (long)(intptr_t)t, sw, sh);
    }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!self.menuView.hidden) return;
    float scale = UIScreen.mainScreen.nativeScale;
    int sw = (int)(self.view.bounds.size.width * scale);
    int sh = (int)(self.view.bounds.size.height * scale);
    for (UITouch* t in touches) {
        CGPoint p = [t locationInView:self.view];
        iOSTouchEnded(p.x * scale, p.y * scale, (long)(intptr_t)t, sw, sh);
    }
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!self.menuView.hidden) return;
    float scale = UIScreen.mainScreen.nativeScale;
    int sw = (int)(self.view.bounds.size.width * scale);
    int sh = (int)(self.view.bounds.size.height * scale);
    iOSTouchCancelled(sw, sh);
}

@end
