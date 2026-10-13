/*
 * ExternalSMBStorage.inc
 *
 * Source-level SMB / File Provider external game support for Madeira on iOS.
 * Included from IOSDisplayShim.m so it is compiled into the normal app target.
 *
 * Goals:
 *  - select a folder from Files (including SMB / File Provider locations)
 *  - keep a security-scoped URL active while Madeira is running
 *  - persist/restore the folder using a bookmark
 *  - expose the selected folder as Wine drive E:
 *  - launch the selected EXE without copying the game tree into Madeira
 *  - diagnose whether direct POSIX access works for the selected provider
 *
 * Important File Provider behavior:
 *  iOS may materialize individual remote files on demand and keep its own system
 *  cache. Madeira never mirrors/copies the whole game folder and creates no
 *  application-side game cache.
 */

@import UIKit;
@import UniformTypeIdentifiers;
#import <Foundation/Foundation.h>
#import <os/log.h>
#import "WineProcessBridge.h"
#import "WineServerBridge.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>

static NSString * const MXSMBBookmarkKey = @"madeira.smb.bookmark.v3";
static NSString * const MXSMBExeKey      = @"madeira.smb.exe.v3";
static NSString * const MXSMBNameKey     = @"madeira.smb.name.v3";

static os_log_t mx_smb_oslog(void)
{
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ log = os_log_create("com.madeira.emulator", "external-smb"); });
    return log;
}

static void mx_smb_log(NSString *tag, NSString *format, ...)
{
    va_list ap;
    va_start(ap, format);
    NSString *body = [[NSString alloc] initWithFormat:format arguments:ap];
    va_end(ap);
    NSString *line = [NSString stringWithFormat:@"[%@] %@", tag, body ?: @""];
    os_log_with_type(mx_smb_oslog(), OS_LOG_TYPE_DEFAULT, "%{public}s", line.UTF8String);
    dprintf(STDERR_FILENO, "%s\n", line.UTF8String);
}

static double mx_smb_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

@interface MXSMBExternalStorage : NSObject <UIDocumentPickerDelegate>
@property(nonatomic,strong) UIButton *button;
@property(nonatomic,strong) NSURL *rootURL;
@property(nonatomic,assign) BOOL scopeActive;
@property(nonatomic,copy) NSString *selectedExeRelative;
@property(nonatomic,strong) NSArray<NSURL *> *lastExecutables;
@property(nonatomic,copy) NSString *legacyShadowPath;
@end

@implementation MXSMBExternalStorage

+ (instancetype)shared
{
    static MXSMBExternalStorage *obj;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        obj = [MXSMBExternalStorage new];
        obj.lastExecutables = @[];
    });
    return obj;
}

- (NSString *)documentsPath
{
    return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
}

- (NSString *)prefixPath
{
    return [[self documentsPath] stringByAppendingPathComponent:@"wine"];
}

- (UIViewController *)topController
{
    UIWindow *key = nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (scene.activationState != UISceneActivationStateForegroundActive) continue;
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        UIWindowScene *ws = (UIWindowScene *)scene;
        for (UIWindow *w in ws.windows) if (w.isKeyWindow) { key = w; break; }
        if (!key) key = ws.windows.firstObject;
        if (key) break;
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (!key) key = UIApplication.sharedApplication.windows.firstObject;
#pragma clang diagnostic pop
    UIViewController *vc = key.rootViewController;
    while (vc.presentedViewController) vc = vc.presentedViewController;
    if ([vc isKindOfClass:UINavigationController.class]) vc = ((UINavigationController *)vc).topViewController;
    if ([vc isKindOfClass:UITabBarController.class]) vc = ((UITabBarController *)vc).selectedViewController;
    return vc;
}

- (UIWindow *)activeWindow
{
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (scene.activationState != UISceneActivationStateForegroundActive) continue;
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        UIWindowScene *ws = (UIWindowScene *)scene;
        for (UIWindow *w in ws.windows) if (w.isKeyWindow) return w;
        return ws.windows.firstObject;
    }
    return nil;
}

- (void)alert:(NSString *)title message:(NSString *)message
{
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *vc = [self topController];
        if (!vc) return;
        UIAlertController *a = [UIAlertController alertControllerWithTitle:title
                                                                   message:message
                                                            preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [vc presentViewController:a animated:YES completion:nil];
    });
}

- (void)installButtonWhenReady:(NSInteger)attempt
{
    dispatch_async(dispatch_get_main_queue(), ^{
        UIWindow *w = [self activeWindow];
        if (!w) {
            if (attempt < 80) {
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.4 * NSEC_PER_SEC)),
                               dispatch_get_main_queue(), ^{ [self installButtonWhenReady:attempt + 1]; });
            }
            return;
        }
        if (self.button.superview) { [w bringSubviewToFront:self.button]; return; }

        UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
        CGFloat width = 136.0;
        b.frame = CGRectMake(MAX(8.0, w.bounds.size.width - width - 10.0),
                             w.safeAreaInsets.top + 8.0, width, 42.0);
        b.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleBottomMargin;
        [b setTitle:@"JEU EXTERNE" forState:UIControlStateNormal];
        b.titleLabel.font = [UIFont boldSystemFontOfSize:11.5];
        b.backgroundColor = [UIColor colorWithRed:0.08 green:0.42 blue:0.95 alpha:0.96];
        [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
        b.layer.cornerRadius = 14.0;
        b.layer.borderWidth = 1.0;
        b.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.30].CGColor;
        b.accessibilityLabel = @"Ajouter un jeu depuis un dossier externe";
        [b addTarget:self action:@selector(showMenu) forControlEvents:UIControlEventTouchUpInside];
        [w addSubview:b];
        [w bringSubviewToFront:b];
        self.button = b;

        mx_smb_log(@"ExternalStorage", @"external-game UI installed");
        [self restoreBookmarkIfPresent];
    });
}

- (void)showMenu
{
    UIViewController *vc = [self topController];
    if (!vc) return;

    NSString *folder = self.rootURL ? self.rootURL.lastPathComponent : @"Aucun dossier";
    NSString *exe = self.selectedExeRelative.length ? self.selectedExeRelative : @"Aucun EXE";
    NSString *state = self.scopeActive ? @"actif" : @"inactif";
    NSString *msg = [NSString stringWithFormat:@"Dossier : %@\nSecurity scope : %@\nLecteur Wine : E:\\\nEXE : %@",
                     folder, state, exe];

    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Stockage externe / SMB"
                                                                   message:msg
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    __weak typeof(self) weakSelf = self;

    [sheet addAction:[UIAlertAction actionWithTitle:@"Ajouter un jeu depuis un dossier externe"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *a) { [weakSelf pickFolder]; }]];

    if (self.rootURL) {
        [sheet addAction:[UIAlertAction actionWithTitle:@"Choisir le .exe"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *a) { [weakSelf scanAndChooseExecutable]; }]];
        [sheet addAction:[UIAlertAction actionWithTitle:@"Tester le stockage externe"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *a) { [weakSelf testStorage]; }]];
        if (self.selectedExeRelative.length) {
            NSString *t = [NSString stringWithFormat:@"Lancer %@", self.selectedExeRelative.lastPathComponent];
            [sheet addAction:[UIAlertAction actionWithTitle:t
                                                      style:UIAlertActionStyleDefault
                                                    handler:^(__unused UIAlertAction *a) { [weakSelf launchSelectedExecutable]; }]];
        }
        [sheet addAction:[UIAlertAction actionWithTitle:@"Reconnecter le dossier"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *a) { [weakSelf pickFolder]; }]];
        [sheet addAction:[UIAlertAction actionWithTitle:@"Retirer le dossier externe"
                                                  style:UIAlertActionStyleDestructive
                                                handler:^(__unused UIAlertAction *a) { [weakSelf removeFolder]; }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Annuler" style:UIAlertActionStyleCancel handler:nil]];

    UIPopoverPresentationController *pop = sheet.popoverPresentationController;
    if (pop) {
        pop.sourceView = self.button ?: vc.view;
        pop.sourceRect = self.button ? self.button.bounds : CGRectMake(1, 1, 1, 1);
    }
    [vc presentViewController:sheet animated:YES completion:nil];
}

- (void)pickFolder
{
    UIViewController *vc = [self topController];
    if (!vc) return;
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
                                               initForOpeningContentTypes:@[UTTypeFolder]
                                               asCopy:NO];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    if (self.rootURL) picker.directoryURL = self.rootURL;
    mx_smb_log(@"ExternalStorage", @"presenting folder picker");
    [vc presentViewController:picker animated:YES completion:nil];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller
{
    (void)controller;
    mx_smb_log(@"ExternalStorage", @"folder picker cancelled");
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
    (void)controller;
    NSURL *url = urls.firstObject;
    if (!url) {
        mx_smb_log(@"ExternalStorage", @"picker returned no URL");
        return;
    }
    NSNumber *isDir = nil;
    NSError *typeError = nil;
    [url getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:&typeError];
    if (!isDir.boolValue) {
        [self alert:@"Dossier requis" message:@"Sélectionne le dossier complet du jeu ou de la bibliothèque Steam."];
        return;
    }
    mx_smb_log(@"ExternalStorage", @"picked folder name=%@ path=%@", url.lastPathComponent, url.path);
    if ([self attachRootURL:url persist:YES]) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.4 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{ [self scanAndChooseExecutable]; });
    }
}

- (BOOL)saveBookmarkForURL:(NSURL *)url
{
    NSError *error = nil;
    NSData *bookmark = [url bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark
                         includingResourceValuesForKeys:nil
                                          relativeToURL:nil
                                                  error:&error];
    if (!bookmark) {
        mx_smb_log(@"SecurityScope", @"bookmark save FAILED: %@", error.localizedDescription ?: @"unknown");
        return NO;
    }
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    [d setObject:bookmark forKey:MXSMBBookmarkKey];
    [d setObject:url.lastPathComponent ?: @"External" forKey:MXSMBNameKey];
    [d synchronize];
    mx_smb_log(@"SecurityScope", @"bookmark saved bytes=%lu", (unsigned long)bookmark.length);
    return YES;
}

- (BOOL)attachRootURL:(NSURL *)url persist:(BOOL)persist
{
    if (!url) return NO;
    if (self.scopeActive && self.rootURL) {
        [self.rootURL stopAccessingSecurityScopedResource];
        mx_smb_log(@"SecurityScope", @"released previous scope");
    }

    BOOL started = [url startAccessingSecurityScopedResource];
    self.rootURL = url;
    self.scopeActive = started;
    mx_smb_log(@"SecurityScope", @"startAccessing=%@ path=%@", started ? @"YES" : @"NO", url.path ?: @"<nil>");
    if (!started) {
        [self alert:@"Accès refusé" message:@"iOS n'a pas accordé l'accès security-scoped au dossier. Reconnecte-le depuis Fichiers."];
        return NO;
    }

    if (persist) [self saveBookmarkForURL:url];
    [self ensureWineMappings];
    return YES;
}

- (void)restoreBookmarkIfPresent
{
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    self.selectedExeRelative = [d stringForKey:MXSMBExeKey];
    NSData *bookmark = [d objectForKey:MXSMBBookmarkKey];
    if (![bookmark isKindOfClass:NSData.class] || bookmark.length == 0) {
        mx_smb_log(@"SecurityScope", @"no saved bookmark");
        return;
    }

    BOOL stale = NO;
    NSError *error = nil;
    NSURL *url = [NSURL URLByResolvingBookmarkData:bookmark
                                           options:NSURLBookmarkResolutionWithoutUI
                                     relativeToURL:nil
                               bookmarkDataIsStale:&stale
                                             error:&error];
    if (!url) {
        mx_smb_log(@"SecurityScope", @"bookmark resolve FAILED: %@", error.localizedDescription ?: @"unknown");
        return;
    }
    mx_smb_log(@"SecurityScope", @"bookmark resolved stale=%@ path=%@", stale ? @"YES" : @"NO", url.path ?: @"<nil>");
    if ([self attachRootURL:url persist:stale]) {
        if (stale) mx_smb_log(@"SecurityScope", @"stale bookmark recreated");
        [self validateStoredExecutable];
    }
}

- (void)validateStoredExecutable
{
    if (!self.rootURL || !self.selectedExeRelative.length) return;
    NSURL *exe = [self.rootURL URLByAppendingPathComponent:self.selectedExeRelative];
    struct stat st;
    if (stat(exe.fileSystemRepresentation, &st) != 0 || !S_ISREG(st.st_mode)) {
        mx_smb_log(@"ExternalStorage", @"stored EXE unavailable: %@ errno=%d", self.selectedExeRelative, errno);
        self.selectedExeRelative = nil;
        [NSUserDefaults.standardUserDefaults removeObjectForKey:MXSMBExeKey];
    }
}

- (NSString *)relativePathForURL:(NSURL *)url
{
    if (!self.rootURL || !url) return nil;
    NSString *root = self.rootURL.standardizedURL.path;
    NSString *path = url.standardizedURL.path;
    NSString *prefix = [root hasSuffix:@"/"] ? root : [root stringByAppendingString:@"/"];
    if (![path hasPrefix:prefix]) return nil;
    return [path substringFromIndex:prefix.length];
}

- (BOOL)ensureWineMappings
{
    if (!self.rootURL || !self.scopeActive) return NO;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *prefix = [self prefixPath];
    NSString *dos = [prefix stringByAppendingPathComponent:@"dosdevices"];
    NSString *driveC = [prefix stringByAppendingPathComponent:@"drive_c"];
    NSError *error = nil;
    [fm createDirectoryAtPath:dos withIntermediateDirectories:YES attributes:nil error:&error];
    [fm createDirectoryAtPath:driveC withIntermediateDirectories:YES attributes:nil error:nil];
    if (error) {
        mx_smb_log(@"WineDrive", @"create dosdevices FAILED: %@", error.localizedDescription);
        return NO;
    }

    NSString *root = self.rootURL.path;
    NSString *eLink = [dos stringByAppendingPathComponent:@"e:"];
    NSString *cur = [fm destinationOfSymbolicLinkAtPath:eLink error:nil];
    if (![cur isEqualToString:root]) {
        [fm removeItemAtPath:eLink error:nil];
        if (![fm createSymbolicLinkAtPath:eLink withDestinationPath:root error:&error]) {
            mx_smb_log(@"WineDrive", @"E: symlink FAILED target=%@ error=%@", root, error.localizedDescription ?: @"unknown");
            return NO;
        }
    }

    /* Compatibility alias for Madeira's current C:-centric host-CWD bridge.
       This is only a symlink; it does not copy a byte of the game. */
    NSString *compat = [driveC stringByAppendingPathComponent:@"__MadeiraExternalE"];
    cur = [fm destinationOfSymbolicLinkAtPath:compat error:nil];
    if (![cur isEqualToString:root]) {
        [fm removeItemAtPath:compat error:nil];
        error = nil;
        if (![fm createSymbolicLinkAtPath:compat withDestinationPath:root error:&error]) {
            mx_smb_log(@"WineDrive", @"compat C: alias FAILED: %@", error.localizedDescription ?: @"unknown");
        }
    }

    setenv("MADEIRA_EXTERNAL_ROOT_E", root.fileSystemRepresentation, 1);
    setenv("MADEIRA_EXTERNAL_DRIVE", "E:", 1);

    struct stat st;
    int rc = stat(eLink.fileSystemRepresentation, &st);
    mx_smb_log(@"WineDrive", @"E: -> %@ stat=%d errno=%d", root, rc, rc ? errno : 0);
    return rc == 0;
}

- (BOOL)prepareLegacyBridgeShadowForRelativeExe:(NSString *)relative
{
    self.legacyShadowPath = nil;
    NSArray<NSString *> *parts = [relative pathComponents];
    if (parts.count < 2) return YES; /* root EXE: pre-chdir below is enough */

    NSString *first = parts.firstObject;
    if (!first.length || [first isEqualToString:@"."] || [first isEqualToString:@".."]) return NO;

    NSString *shadow = [[[self prefixPath] stringByAppendingPathComponent:@"drive_c"] stringByAppendingPathComponent:first];
    NSString *target = [self.rootURL.path stringByAppendingPathComponent:first];
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *existing = [fm destinationOfSymbolicLinkAtPath:shadow error:nil];
    if (existing) {
        if ([existing isEqualToString:target]) { self.legacyShadowPath = shadow; return YES; }
        [fm removeItemAtPath:shadow error:nil];
    } else if ([fm fileExistsAtPath:shadow]) {
        mx_smb_log(@"WineDrive", @"legacy CWD shadow collision at C:\\%@; will use compatibility C: alias", first);
        return NO;
    }

    NSError *error = nil;
    if ([fm createSymbolicLinkAtPath:shadow withDestinationPath:target error:&error]) {
        self.legacyShadowPath = shadow;
        mx_smb_log(@"WineDrive", @"legacy CWD shadow %@ -> %@", shadow, target);
        return YES;
    }
    mx_smb_log(@"WineDrive", @"legacy CWD shadow FAILED: %@", error.localizedDescription ?: @"unknown");
    return NO;
}

- (NSArray<NSURL *> *)directScanExecutables:(BOOL *)hadErrorOut
{
    __block BOOL hadError = NO;
    NSMutableArray<NSURL *> *found = [NSMutableArray array];
    NSArray *keys = @[NSURLIsDirectoryKey, NSURLIsRegularFileKey, NSURLFileSizeKey];
    NSDirectoryEnumerator *e = [NSFileManager.defaultManager enumeratorAtURL:self.rootURL
                                                  includingPropertiesForKeys:keys
                                                                     options:NSDirectoryEnumerationSkipsHiddenFiles
                                                                errorHandler:^BOOL(NSURL *u, NSError *err) {
        hadError = YES;
        mx_smb_log(@"FileAccess", @"direct enumeration error %@: %@", u.lastPathComponent, err.localizedDescription);
        return YES;
    }];
    NSUInteger visited = 0;
    for (NSURL *u in e) {
        if (++visited > 40000 || found.count >= 200) break;
        NSNumber *isDir = nil;
        [u getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
        if (isDir.boolValue) {
            NSString *rel = [self relativePathForURL:u];
            if ([[rel pathComponents] count] > 12) [e skipDescendants];
            continue;
        }
        if ([u.pathExtension.lowercaseString isEqualToString:@"exe"]) [found addObject:u];
    }
    mx_smb_log(@"FileAccess", @"direct EXE metadata scan visited=%lu found=%lu", (unsigned long)visited, (unsigned long)found.count);
    if (hadErrorOut) *hadErrorOut = hadError;
    return found;
}

- (NSArray<NSURL *> *)scanExecutablesSynchronously
{
    if (!self.rootURL || !self.scopeActive) return @[];
    BOOL hadError = NO;
    NSMutableArray<NSURL *> *found = [[self directScanExecutables:&hadError] mutableCopy];

    /* Apple recommends coordination for external documents. We deliberately
       try direct metadata enumeration first because Wine itself uses POSIX I/O.
       Coordinator fallback is metadata-only and does not copy the tree. */
    if (found.count == 0 && hadError) {
        mx_smb_log(@"FileAccess", @"direct scan failed; trying metadata-only NSFileCoordinator fallback");
        NSFileCoordinator *coord = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
        __block NSArray<NSURL *> *coordinated = @[];
        __block NSError *coordError = nil;
        [coord coordinateReadingItemAtURL:self.rootURL
                                  options:NSFileCoordinatorReadingWithoutChanges
                                    error:&coordError
                               byAccessor:^(NSURL *newURL) {
            NSURL *old = self.rootURL;
            self.rootURL = newURL;
            BOOL ignored = NO;
            coordinated = [self directScanExecutables:&ignored];
            self.rootURL = old;
        }];
        if (coordError) mx_smb_log(@"FileAccess", @"coordinator fallback FAILED: %@", coordError.localizedDescription);
        [found addObjectsFromArray:coordinated];
    }

    [found sortUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
        NSString *an = a.lastPathComponent.lowercaseString;
        NSString *bn = b.lastPathComponent.lowercaseString;
        int ap = [an isEqualToString:@"launcher.exe"] ? 0 : (([an containsString:@"tlou"] || [an containsString:@"lastofus"]) ? 1 : 2);
        int bp = [bn isEqualToString:@"launcher.exe"] ? 0 : (([bn containsString:@"tlou"] || [bn containsString:@"lastofus"]) ? 1 : 2);
        if (ap != bp) return ap < bp ? NSOrderedAscending : NSOrderedDescending;
        return [an localizedStandardCompare:bn];
    }];
    return found;
}

- (void)scanAndChooseExecutable
{
    if (!self.rootURL || !self.scopeActive) {
        [self alert:@"Dossier externe" message:@"Ajoute ou reconnecte d'abord le dossier du jeu."];
        return;
    }
    [self ensureWineMappings];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSArray<NSURL *> *exes = [self scanExecutablesSynchronously];
        self.lastExecutables = exes;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (exes.count == 0) {
                [self alert:@"Aucun .exe" message:@"Aucun exécutable n'a été trouvé. Lance “Tester le stockage externe” pour voir si le provider SMB accepte les accès directs."];
                return;
            }
            UIViewController *vc = [self topController];
            if (!vc) return;
            UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Choisir le .exe"
                                                                           message:[NSString stringWithFormat:@"%lu exécutable(s). Aucun fichier n'est copié dans Madeira.", (unsigned long)exes.count]
                                                                    preferredStyle:UIAlertControllerStyleActionSheet];
            __weak typeof(self) weakSelf = self;
            NSUInteger limit = MIN((NSUInteger)40, exes.count);
            for (NSUInteger i = 0; i < limit; i++) {
                NSURL *u = exes[i];
                NSString *rel = [self relativePathForURL:u] ?: u.lastPathComponent;
                NSString *label = rel.length > 80 ? [@"…" stringByAppendingString:[rel substringFromIndex:rel.length - 79]] : rel;
                [sheet addAction:[UIAlertAction actionWithTitle:label style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
                    [weakSelf selectExecutableURL:u];
                }]];
            }
            [sheet addAction:[UIAlertAction actionWithTitle:@"Annuler" style:UIAlertActionStyleCancel handler:nil]];
            UIPopoverPresentationController *pop = sheet.popoverPresentationController;
            if (pop) { pop.sourceView = self.button ?: vc.view; pop.sourceRect = self.button ? self.button.bounds : CGRectMake(1,1,1,1); }
            [vc presentViewController:sheet animated:YES completion:nil];
        });
    });
}

- (void)selectExecutableURL:(NSURL *)url
{
    NSString *rel = [self relativePathForURL:url];
    if (!rel.length) {
        [self alert:@"EXE invalide" message:@"L'exécutable n'est pas dans le dossier externe sélectionné."];
        return;
    }
    self.selectedExeRelative = rel;
    [NSUserDefaults.standardUserDefaults setObject:rel forKey:MXSMBExeKey];
    NSString *win = [[NSString stringWithFormat:@"E:\\%@", rel] stringByReplacingOccurrencesOfString:@"/" withString:@"\\"];
    mx_smb_log(@"ExternalStorage", @"selected EXE=%@ Wine=%@", rel, win);
    [self alert:@"Jeu externe prêt" message:[NSString stringWithFormat:@"EXE : %@\nLecteur : %@\n\nLa source reste le dossier SMB/File Provider.", rel, win]];
}

- (NSURL *)bestTestFile
{
    if (self.selectedExeRelative.length) {
        NSURL *u = [self.rootURL URLByAppendingPathComponent:self.selectedExeRelative];
        struct stat st;
        if (stat(u.fileSystemRepresentation, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) return u;
    }

    NSDirectoryEnumerator *e = [NSFileManager.defaultManager enumeratorAtURL:self.rootURL
                                                  includingPropertiesForKeys:@[NSURLFileSizeKey, NSURLIsRegularFileKey]
                                                                     options:NSDirectoryEnumerationSkipsHiddenFiles
                                                                errorHandler:nil];
    NSURL *fallback = nil;
    NSUInteger seen = 0;
    for (NSURL *u in e) {
        if (++seen > 3500) break;
        NSNumber *regular = nil, *size = nil;
        [u getResourceValue:&regular forKey:NSURLIsRegularFileKey error:nil];
        if (!regular.boolValue) continue;
        [u getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
        unsigned long long n = size.unsignedLongLongValue;
        if (!fallback && n > 0) fallback = u;
        if (n >= (8ULL << 20) && n <= (128ULL << 20)) return u;
    }
    return fallback;
}

- (NSDictionary *)directPOSIXProbe:(NSURL *)file
{
    if (!file) return @{ @"read": @NO, @"mmap": @NO, @"size": @0, @"bytes": @0, @"latency": @0.0, @"speed": @0.0 };
    struct stat st;
    unsigned long long size = 0;
    if (stat(file.fileSystemRepresentation, &st) == 0) size = (unsigned long long)MAX((off_t)0, st.st_size);

    double t0 = mx_smb_now();
    int fd = open(file.fileSystemRepresentation, O_RDONLY);
    if (fd < 0) {
        mx_smb_log(@"FileAccess", @"direct open FAILED path=%@ errno=%d", file.path, errno);
        return @{ @"read": @NO, @"mmap": @NO, @"size": @(size), @"bytes": @0, @"latency": @0.0, @"speed": @0.0 };
    }

    char first[4096];
    ssize_t firstN = read(fd, first, sizeof(first));
    double latency = (mx_smb_now() - t0) * 1000.0;
    BOOL readable = firstN >= 0;
    BOOL mmapOK = NO;
    if (size > 0) {
        size_t len = (size_t)MIN((unsigned long long)4096, size);
        void *p = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p != MAP_FAILED) {
            volatile unsigned char x = *((unsigned char *)p); (void)x;
            mmapOK = YES;
            munmap(p, len);
        }
    }

    lseek(fd, 0, SEEK_SET);
    const size_t chunk = 1024 * 1024;
    unsigned long long maxRead = MIN(size, 16ULL << 20);
    unsigned long long bytes = 0;
    char *buf = malloc(chunk);
    double r0 = mx_smb_now();
    while (buf && bytes < maxRead) {
        size_t want = (size_t)MIN((unsigned long long)chunk, maxRead - bytes);
        ssize_t n = read(fd, buf, want);
        if (n <= 0) break;
        bytes += (unsigned long long)n;
    }
    double elapsed = mx_smb_now() - r0;
    if (buf) free(buf);
    close(fd);
    double speed = elapsed > 0 && bytes > 0 ? ((double)bytes / (1024.0 * 1024.0)) / elapsed : 0.0;

    mx_smb_log(@"FileAccess", @"direct POSIX read=%@ mmap=%@ size=%llu bytes=%llu latency=%.2fms speed=%.1fMiB/s",
               readable ? @"YES" : @"NO", mmapOK ? @"YES" : @"NO", size, bytes, latency, speed);
    return @{ @"read": @(readable), @"mmap": @(mmapOK), @"size": @(size), @"bytes": @(bytes), @"latency": @(latency), @"speed": @(speed) };
}

- (BOOL)coordinatedSingleFileProbe:(NSURL *)file
{
    if (!file) return NO;
    NSFileCoordinator *coord = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    __block BOOL ok = NO;
    __block NSError *error = nil;
    [coord coordinateReadingItemAtURL:file
                              options:NSFileCoordinatorReadingWithoutChanges
                                error:&error
                           byAccessor:^(NSURL *u) {
        int fd = open(u.fileSystemRepresentation, O_RDONLY);
        if (fd >= 0) { char b; ok = read(fd, &b, 1) >= 0; close(fd); }
    }];
    mx_smb_log(@"FileAccess", @"single-file coordinator fallback=%@ error=%@", ok ? @"YES" : @"NO", error.localizedDescription ?: @"none");
    return ok;
}

- (void)testStorage
{
    if (!self.rootURL || !self.scopeActive) {
        [self alert:@"Test stockage externe" message:@"Aucun dossier externe actif."];
        return;
    }
    NSURL *root = self.rootURL;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        struct stat rst;
        errno = 0;
        BOOL statOK = stat(root.fileSystemRepresentation, &rst) == 0 && S_ISDIR(rst.st_mode);
        DIR *d = opendir(root.fileSystemRepresentation);
        BOOL dirOK = d != NULL;
        if (d) closedir(d);
        mx_smb_log(@"SMB", @"root direct stat=%@ opendir=%@ path=%@", statOK ? @"YES" : @"NO", dirOK ? @"YES" : @"NO", root.path);

        NSURL *file = [self bestTestFile];
        NSDictionary *probe = [self directPOSIXProbe:file];
        BOOL directRead = [probe[@"read"] boolValue];
        BOOL coordinated = directRead ? NO : [self coordinatedSingleFileProbe:file];

        BOOL writable = NO;
        NSString *tmpName = [NSString stringWithFormat:@".madeira-write-test-%u.tmp", arc4random()];
        NSURL *tmp = [root URLByAppendingPathComponent:tmpName];
        int wfd = open(tmp.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (wfd >= 0) {
            char bytes[4096]; memset(bytes, 0x4d, sizeof(bytes));
            writable = write(wfd, bytes, sizeof(bytes)) == sizeof(bytes);
            close(wfd);
            unlink(tmp.fileSystemRepresentation);
        }
        mx_smb_log(@"FileAccess", @"direct write=%@", writable ? @"YES" : @"NO");

        NSString *steamapps = [root.path stringByAppendingPathComponent:@"steamapps"];
        BOOL steam = [NSFileManager.defaultManager fileExistsAtPath:steamapps];
        NSUInteger manifests = 0;
        if (steam) {
            for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:steamapps error:nil]) {
                if ([name hasPrefix:@"appmanifest_"] && [name hasSuffix:@".acf"]) manifests++;
            }
            mx_smb_log(@"SMB", @"Steam library detected manifests=%lu", (unsigned long)manifests);
        }

        NSString *compat = directRead ? @"OUI — compatible avec les accès POSIX de Wine (à confirmer en jeu)" :
                          (coordinated ? @"NON direct — le provider exige une coordination; Wine direct E: risque d'échouer" : @"NON — fichier non lisible");
        NSString *result = [NSString stringWithFormat:
            @"Dossier accessible : %@\nÉnumération : %@\nLecture POSIX directe : %@\nÉcriture directe : %@\nmmap : %@\nCompatibilité Wine directe : %@\nChemin : %@\nFichier test : %@\nTaille : %.2f MiB\nLecture test : %.2f MiB\nDébit séquentiel approx. : %.1f MiB/s\nLatence approx. : %.2f ms\nSteamapps : %@%@\n\nAucune copie complète n'a été créée par Madeira.",
            statOK ? @"oui" : @"non",
            dirOK ? @"oui" : @"non",
            directRead ? @"oui" : @"non",
            writable ? @"oui" : @"non",
            [probe[@"mmap"] boolValue] ? @"oui" : @"non",
            compat,
            root.path ?: @"<indisponible>",
            file.lastPathComponent ?: @"aucun",
            [probe[@"size"] doubleValue] / (1024.0 * 1024.0),
            [probe[@"bytes"] doubleValue] / (1024.0 * 1024.0),
            [probe[@"speed"] doubleValue],
            [probe[@"latency"] doubleValue],
            steam ? @"oui" : @"non",
            steam ? [NSString stringWithFormat:@" (%lu manifestes)", (unsigned long)manifests] : @""];
        [self alert:@"Test stockage externe" message:result];
    });
}

- (void)launchSelectedExecutable
{
    if (!self.rootURL || !self.scopeActive || !self.selectedExeRelative.length) {
        [self alert:@"Lancement externe" message:@"Sélectionne d'abord un dossier puis un .exe."];
        return;
    }
    if (wine_process_is_running()) {
        [self alert:@"Wine déjà actif" message:@"Ferme la session Wine actuelle avant de lancer ce jeu."];
        return;
    }
    if (![self ensureWineMappings]) {
        [self alert:@"Lecteur E:" message:@"Impossible de préparer le lecteur externe."];
        return;
    }

    NSURL *exeURL = [self.rootURL URLByAppendingPathComponent:self.selectedExeRelative];
    NSDictionary *probe = [self directPOSIXProbe:exeURL];
    if (![probe[@"read"] boolValue]) {
        BOOL coordinated = [self coordinatedSingleFileProbe:exeURL];
        NSString *m = coordinated
            ? @"Le provider peut lire ce fichier seulement via coordination. Wine utilise des accès POSIX directs, donc lancer maintenant risquerait d'échouer. Aucun fichier n'a été copié."
            : @"L'EXE n'est pas lisible directement depuis le provider SMB.";
        [self alert:@"SMB non compatible en accès direct" message:m];
        return;
    }

    NSString *relWin = [self.selectedExeRelative stringByReplacingOccurrencesOfString:@"/" withString:@"\\"];
    NSString *eExe = [NSString stringWithFormat:@"E:\\%@", relWin];
    NSString *relDir = [relWin stringByDeletingLastPathComponent];
    NSString *eDir = (relDir.length && ![relDir isEqualToString:@"."])
        ? [NSString stringWithFormat:@"E:\\%@", relDir]
        : @"E:\\";
    NSURL *hostDir = [exeURL URLByDeletingLastPathComponent];

    BOOL canUseEPath = [self prepareLegacyBridgeShadowForRelativeExe:self.selectedExeRelative];
    NSString *launchExe = eExe;
    NSString *launchDir = eDir;
    if (!canUseEPath) {
        launchExe = [NSString stringWithFormat:@"C:\\__MadeiraExternalE\\%@", relWin];
        launchDir = (relDir.length && ![relDir isEqualToString:@"."])
            ? [NSString stringWithFormat:@"C:\\__MadeiraExternalE\\%@", relDir]
            : @"C:\\__MadeiraExternalE\\";
        mx_smb_log(@"WineDrive", @"using compatibility C: launch alias due to shadow collision; E: remains mounted");
    }

    setenv("MADEIRA_EXE", launchExe.UTF8String, 1);
    setenv("MADEIRA_WORKDIR", launchDir.UTF8String, 1);
    setenv("MADEIRA_INITIAL_CWD", eDir.UTF8String, 1);
    setenv("MADEIRA_HOST_WORKDIR", hostDir.path.fileSystemRepresentation, 1);
    setenv("MADEIRA_USE_ARM64EC", "1", 1);
    setenv("MADEIRA_EXTERNAL_ROOT_E", self.rootURL.path.fileSystemRepresentation, 1);
    setenv("SteamAppPath", eDir.UTF8String, 1);
    unsetenv("MADEIRA_ARGS");

    int cwdRC = chdir(hostDir.fileSystemRepresentation);
    if (cwdRC == 0) setenv("PWD", hostDir.path.fileSystemRepresentation, 1);
    mx_smb_log(@"WineDrive", @"launch display=%@ actual=%@ cwd=%@ host=%@ chdir=%d errno=%d",
               eExe, launchExe, eDir, hostDir.path, cwdRC, cwdRC ? errno : 0);
    mx_smb_log(@"SMB", @"source stays remote/File Provider; no Madeira-side game copy");

    NSString *prefix = [self prefixPath];
    if (!wineserver_is_running()) {
        int rc = wineserver_start(prefix.fileSystemRepresentation);
        if (rc != 0) {
            [self alert:@"wineserver" message:[NSString stringWithFormat:@"Échec wineserver (%d)", rc]];
            return;
        }
    }

    __block int attempts = 0;
    __weak typeof(self) weakSelf = self;
    __block void (^poll)(void) = nil;
    poll = ^{
        attempts++;
        if (wineserver_is_running()) {
            int rc = wine_process_start(prefix.fileSystemRepresentation);
            mx_smb_log(@"WineDrive", @"wine_process_start=%d", rc);
            if (rc != 0) {
                [weakSelf alert:@"Wine" message:[NSString stringWithFormat:@"wine_process_start=%d", rc]];
            } else {
                [weakSelf alert:@"Lancement SMB"
                         message:[NSString stringWithFormat:@"%@\n\nLecteur externe : E:\\\nDossier : %@\n\nLe security scope reste actif; Madeira ne copie pas le dossier du jeu.", eExe, weakSelf.rootURL.lastPathComponent]];
            }
            poll = nil;
            return;
        }
        if (attempts >= 150) {
            [weakSelf alert:@"wineserver" message:@"Le serveur Wine n'est pas devenu prêt."];
            poll = nil;
            return;
        }
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.10 * NSEC_PER_SEC)), dispatch_get_main_queue(), poll);
    };
    dispatch_async(dispatch_get_main_queue(), poll);
}

- (void)removeFolder
{
    if (wine_process_is_running()) {
        [self alert:@"Jeu en cours" message:@"Ferme le jeu avant de retirer le dossier externe."];
        return;
    }
    if (self.scopeActive && self.rootURL) [self.rootURL stopAccessingSecurityScopedResource];
    self.scopeActive = NO;
    self.rootURL = nil;
    self.selectedExeRelative = nil;
    self.lastExecutables = @[];

    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    [d removeObjectForKey:MXSMBBookmarkKey];
    [d removeObjectForKey:MXSMBExeKey];
    [d removeObjectForKey:MXSMBNameKey];

    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *prefix = [self prefixPath];
    [fm removeItemAtPath:[[prefix stringByAppendingPathComponent:@"dosdevices"] stringByAppendingPathComponent:@"e:"] error:nil];
    [fm removeItemAtPath:[[prefix stringByAppendingPathComponent:@"drive_c"] stringByAppendingPathComponent:@"__MadeiraExternalE"] error:nil];
    if (self.legacyShadowPath.length) [fm removeItemAtPath:self.legacyShadowPath error:nil];
    self.legacyShadowPath = nil;

    unsetenv("MADEIRA_EXTERNAL_ROOT_E");
    unsetenv("MADEIRA_EXTERNAL_DRIVE");
    mx_smb_log(@"SecurityScope", @"folder removed and scope released");
    [self alert:@"Dossier retiré" message:@"Le bookmark et le lecteur E: ont été retirés. Madeira n'a supprimé aucun fichier du serveur."];
}

- (void)applicationWillTerminate:(NSNotification *)n
{
    (void)n;
    if (self.scopeActive && self.rootURL) {
        [self.rootURL stopAccessingSecurityScopedResource];
        self.scopeActive = NO;
        mx_smb_log(@"SecurityScope", @"scope released on app termination");
    }
}

- (void)start
{
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(applicationWillTerminate:)
                                               name:UIApplicationWillTerminateNotification
                                             object:nil];
    [self installButtonWhenReady:0];
}

@end

__attribute__((constructor))
static void MXInstallSMBExternalStorage(void)
{
    dispatch_async(dispatch_get_main_queue(), ^{ [[MXSMBExternalStorage shared] start]; });
}
