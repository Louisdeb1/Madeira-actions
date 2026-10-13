/*
 * ExternalSMBStorage.inc
 *
 * Source-level external-folder support for Madeira on iOS.
 * This file is intentionally included from WineProcessBridge.m so it is
 * compiled into the normal app target without adding a second binary layer.
 *
 * Design:
 *  - UIDocumentPickerViewController selects a folder from Files / File Provider.
 *  - The returned security-scoped URL is kept open for the Wine session.
 *  - A minimal bookmark is persisted and resolved at next app launch.
 *  - The folder is exposed to Wine as E: through WINEPREFIX/dosdevices/e:.
 *  - No game folder is copied into Madeira. File Provider/iOS may materialize
 *    individual remote files on demand; Madeira itself never mirrors the tree.
 */

@import UIKit;
@import UniformTypeIdentifiers;
@import QuartzCore;
#import <Foundation/Foundation.h>
#import <os/log.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>

static NSString * const MXExtBookmarkKey = @"madeira.external.bookmark.v2";
static NSString * const MXExtDisplayNameKey = @"madeira.external.displayName.v2";
static NSString * const MXExtExeRelativeKey = @"madeira.external.exeRelative.v2";
static NSString * const MXExtDriveKey = @"madeira.external.drive.v2";

static os_log_t mx_ext_log(void)
{
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ log = os_log_create("com.madeira.emulator", "external-storage"); });
    return log;
}

static void mx_log(NSString *tag, NSString *format, ...)
{
    va_list ap;
    va_start(ap, format);
    NSString *body = [[NSString alloc] initWithFormat:format arguments:ap];
    va_end(ap);
    NSString *line = [NSString stringWithFormat:@"[%@] %@", tag, body ?: @""];
    os_log_with_type(mx_ext_log(), OS_LOG_TYPE_DEFAULT, "%{public}s", line.UTF8String);
    dprintf(STDERR_FILENO, "%s\n", line.UTF8String);
}

static double mx_now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

@interface MXExternalSMBStorage : NSObject <UIDocumentPickerDelegate>
@property(nonatomic,strong) UIButton *button;
@property(nonatomic,strong) NSURL *rootURL;
@property(nonatomic,assign) BOOL scopeActive;
@property(nonatomic,copy) NSString *selectedExeRelative;
@property(nonatomic,copy) NSString *driveLetter;
@property(nonatomic,strong) NSArray<NSURL *> *lastExecutables;
@end

@implementation MXExternalSMBStorage

+ (instancetype)shared
{
    static MXExternalSMBStorage *obj;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        obj = [MXExternalSMBStorage new];
        obj.driveLetter = @"E";
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
        for (UIWindow *w in ((UIWindowScene *)scene).windows) if (w.isKeyWindow) return w;
        return ((UIWindowScene *)scene).windows.firstObject;
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
            if (attempt < 60) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                [self installButtonWhenReady:attempt + 1];
            });
            return;
        }
        if (self.button.superview) { [w bringSubviewToFront:self.button]; return; }

        UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
        CGFloat width = 126.0;
        b.frame = CGRectMake(MAX(8.0, w.bounds.size.width - width - 10.0), w.safeAreaInsets.top + 8.0, width, 42.0);
        b.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleBottomMargin;
        [b setTitle:@"JEU EXTERNE" forState:UIControlStateNormal];
        b.titleLabel.font = [UIFont boldSystemFontOfSize:11.5];
        b.backgroundColor = [UIColor colorWithRed:0.08 green:0.42 blue:0.95 alpha:0.96];
        [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
        b.layer.cornerRadius = 14.0;
        b.layer.borderWidth = 1.0;
        b.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.30].CGColor;
        b.accessibilityLabel = @"Ajouter un jeu depuis un dossier externe";
        [b addTarget:self action:@selector(showExternalMenu) forControlEvents:UIControlEventTouchUpInside];
        [w addSubview:b];
        [w bringSubviewToFront:b];
        self.button = b;

        mx_log(@"ExternalStorage", @"UI installed");
        [self restoreBookmarkIfPresent];
    });
}

- (void)showExternalMenu
{
    UIViewController *vc = [self topController];
    if (!vc) return;

    NSString *rootText = self.rootURL ? self.rootURL.lastPathComponent : @"Aucun dossier sélectionné";
    NSString *exeText = self.selectedExeRelative.length ? self.selectedExeRelative : @"Aucun EXE choisi";
    NSString *msg = [NSString stringWithFormat:@"Dossier : %@\nLecteur Wine : %@:\\\nEXE : %@", rootText, self.driveLetter ?: @"E", exeText];
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Stockage externe / SMB"
                                                                   message:msg
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    __weak typeof(self) weakSelf = self;
    [sheet addAction:[UIAlertAction actionWithTitle:@"Ajouter un jeu depuis un dossier externe"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *a) { [weakSelf pickExternalFolder]; }]];
    if (self.rootURL) {
        [sheet addAction:[UIAlertAction actionWithTitle:@"Choisir le .exe"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *a) { [weakSelf scanAndChooseExecutable]; }]];
        [sheet addAction:[UIAlertAction actionWithTitle:@"Tester le stockage externe"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *a) { [weakSelf testStorage]; }]];
        if (self.selectedExeRelative.length) {
            NSString *launchTitle = [NSString stringWithFormat:@"Lancer %@", self.selectedExeRelative.lastPathComponent];
            [sheet addAction:[UIAlertAction actionWithTitle:launchTitle
                                                      style:UIAlertActionStyleDefault
                                                    handler:^(__unused UIAlertAction *a) { [weakSelf launchSelectedExecutable]; }]];
        }
        [sheet addAction:[UIAlertAction actionWithTitle:@"Reconnecter le dossier"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *a) { [weakSelf pickExternalFolder]; }]];
        [sheet addAction:[UIAlertAction actionWithTitle:@"Retirer le dossier externe"
                                                  style:UIAlertActionStyleDestructive
                                                handler:^(__unused UIAlertAction *a) { [weakSelf removeExternalFolder]; }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Annuler" style:UIAlertActionStyleCancel handler:nil]];
    UIPopoverPresentationController *pop = sheet.popoverPresentationController;
    if (pop) {
        pop.sourceView = self.button ?: vc.view;
        pop.sourceRect = self.button ? self.button.bounds : CGRectMake(vc.view.bounds.size.width - 1, 1, 1, 1);
    }
    [vc presentViewController:sheet animated:YES completion:nil];
}

- (void)pickExternalFolder
{
    UIViewController *vc = [self topController];
    if (!vc) return;
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
        initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    if (self.rootURL) picker.directoryURL = self.rootURL;
    mx_log(@"ExternalStorage", @"presenting folder picker (Files/File Provider)");
    [vc presentViewController:picker animated:YES completion:nil];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller
{
    mx_log(@"ExternalStorage", @"folder picker cancelled");
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
    NSURL *url = urls.firstObject;
    if (!url) {
        mx_log(@"ExternalStorage", @"folder picker callback returned no URL");
        return;
    }
    mx_log(@"ExternalStorage", @"folder picker callback: %@", url.lastPathComponent);

    NSNumber *isDirectory = nil;
    [url getResourceValue:&isDirectory forKey:NSURLIsDirectoryKey error:nil];
    if (!isDirectory.boolValue) {
        [self alert:@"Dossier requis" message:@"Sélectionne le dossier complet du jeu ou de la bibliothèque Steam, pas un fichier."];
        return;
    }

    [self attachRootURL:url persist:YES];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.45 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [self scanAndChooseExecutable];
    });
}

- (BOOL)attachRootURL:(NSURL *)url persist:(BOOL)persist
{
    if (!url) return NO;
    if (self.scopeActive && self.rootURL) {
        [self.rootURL stopAccessingSecurityScopedResource];
        mx_log(@"SecurityScope", @"released previous folder scope");
    }

    BOOL started = [url startAccessingSecurityScopedResource];
    self.rootURL = url;
    self.scopeActive = started;
    mx_log(@"SecurityScope", @"startAccessingSecurityScopedResource=%@ path=%@",
           started ? @"YES" : @"NO", url.path ?: @"<nil>");
    if (!started) {
        [self alert:@"Accès refusé" message:@"iOS n'a pas accordé le security scope pour ce dossier. Reconnecte-le depuis Fichiers."];
        return NO;
    }

    if (persist) {
        NSError *bookmarkError = nil;
        NSData *bookmark = [url bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark
                           includingResourceValuesForKeys:nil
                                            relativeToURL:nil
                                                    error:&bookmarkError];
        if (bookmark) {
            NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
            [d setObject:bookmark forKey:MXExtBookmarkKey];
            [d setObject:url.lastPathComponent ?: @"External" forKey:MXExtDisplayNameKey];
            [d setObject:self.driveLetter ?: @"E" forKey:MXExtDriveKey];
            [d synchronize];
            mx_log(@"SecurityScope", @"bookmark saved (%lu bytes)", (unsigned long)bookmark.length);
        } else {
            mx_log(@"SecurityScope", @"bookmark save FAILED: %@", bookmarkError.localizedDescription ?: @"unknown");
        }
    }

    [self ensureWineDriveMapping];
    return YES;
}

- (void)restoreBookmarkIfPresent
{
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    NSData *bookmark = [d objectForKey:MXExtBookmarkKey];
    self.selectedExeRelative = [d stringForKey:MXExtExeRelativeKey];
    NSString *drive = [d stringForKey:MXExtDriveKey];
    if (drive.length == 1) self.driveLetter = drive.uppercaseString;
    if (![bookmark isKindOfClass:NSData.class] || bookmark.length == 0) {
        mx_log(@"SecurityScope", @"no saved bookmark");
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
        mx_log(@"SecurityScope", @"bookmark resolve FAILED: %@", error.localizedDescription ?: @"unknown");
        return;
    }
    mx_log(@"SecurityScope", @"bookmark resolved stale=%@ path=%@", stale ? @"YES" : @"NO", url.path ?: @"<nil>");
    if ([self attachRootURL:url persist:stale]) {
        if (stale) mx_log(@"SecurityScope", @"stale bookmark recreated");
        [self validateStoredExecutable];
    }
}

- (void)validateStoredExecutable
{
    if (!self.rootURL || !self.selectedExeRelative.length) return;
    NSURL *exe = [self.rootURL URLByAppendingPathComponent:self.selectedExeRelative];
    if (![NSFileManager.defaultManager fileExistsAtPath:exe.path]) {
        mx_log(@"ExternalStorage", @"stored EXE missing: %@", self.selectedExeRelative);
        self.selectedExeRelative = nil;
        [NSUserDefaults.standardUserDefaults removeObjectForKey:MXExtExeRelativeKey];
    }
}

- (void)removeExternalFolder
{
    if (wine_process_is_running()) {
        [self alert:@"Jeu en cours" message:@"Ferme le jeu avant de retirer le dossier externe."];
        return;
    }
    if (self.scopeActive && self.rootURL) {
        [self.rootURL stopAccessingSecurityScopedResource];
        mx_log(@"SecurityScope", @"stopAccessingSecurityScopedResource");
    }
    self.scopeActive = NO;
    self.rootURL = nil;
    self.selectedExeRelative = nil;
    self.lastExecutables = @[];
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    [d removeObjectForKey:MXExtBookmarkKey];
    [d removeObjectForKey:MXExtDisplayNameKey];
    [d removeObjectForKey:MXExtExeRelativeKey];
    [d removeObjectForKey:MXExtDriveKey];

    NSString *link = [[[self prefixPath] stringByAppendingPathComponent:@"dosdevices"] stringByAppendingPathComponent:@"e:"];
    [NSFileManager.defaultManager removeItemAtPath:link error:nil];
    unsetenv("MADEIRA_EXTERNAL_ROOT_E");
    unsetenv("MADEIRA_EXTERNAL_DRIVE");
    mx_log(@"WineDrive", @"E: mapping removed");
    [self alert:@"Dossier retiré" message:@"Le bookmark et le lecteur E: ont été supprimés. Aucun fichier du jeu n'a été copié ni supprimé."];
}

- (BOOL)ensureWineDriveMapping
{
    if (!self.rootURL || !self.scopeActive) return NO;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *dosdevices = [[self prefixPath] stringByAppendingPathComponent:@"dosdevices"];
    NSError *error = nil;
    [fm createDirectoryAtPath:dosdevices withIntermediateDirectories:YES attributes:nil error:&error];
    if (error) {
        mx_log(@"WineDrive", @"create dosdevices FAILED: %@", error.localizedDescription);
        return NO;
    }

    NSString *drive = [NSString stringWithFormat:@"%@:", (self.driveLetter ?: @"E").lowercaseString];
    NSString *link = [dosdevices stringByAppendingPathComponent:drive];
    NSString *wanted = self.rootURL.path;
    NSString *current = [fm destinationOfSymbolicLinkAtPath:link error:nil];
    if (![current isEqualToString:wanted]) {
        [fm removeItemAtPath:link error:nil];
        error = nil;
        if (![fm createSymbolicLinkAtPath:link withDestinationPath:wanted error:&error]) {
            mx_log(@"WineDrive", @"symlink %@ -> %@ FAILED: %@", drive, wanted, error.localizedDescription ?: @"unknown");
            [self alert:@"Lecteur Wine" message:[NSString stringWithFormat:@"Impossible de créer %@ -> dossier externe : %@", drive.uppercaseString, error.localizedDescription ?: @"erreur"]];
            return NO;
        }
    }

    setenv("MADEIRA_EXTERNAL_ROOT_E", wanted.fileSystemRepresentation, 1);
    setenv("MADEIRA_EXTERNAL_DRIVE", "E:", 1);
    struct stat st;
    int rc = stat(link.fileSystemRepresentation, &st);
    mx_log(@"WineDrive", @"mapped E: -> %@ stat=%d errno=%d", wanted, rc, rc ? errno : 0);
    return rc == 0;
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

- (NSArray<NSURL *> *)scanExecutablesSynchronously
{
    if (!self.rootURL || !self.scopeActive) return @[];
    NSMutableArray<NSURL *> *found = [NSMutableArray array];
    NSFileManager *fm = NSFileManager.defaultManager;
    NSArray *keys = @[NSURLIsRegularFileKey, NSURLIsDirectoryKey, NSURLFileSizeKey];
    __block NSError *enumError = nil;

    void (^enumerateBlock)(NSURL *) = ^(NSURL *coordinatedRoot) {
        NSDirectoryEnumerator *e = [fm enumeratorAtURL:coordinatedRoot
                           includingPropertiesForKeys:keys
                                              options:(NSDirectoryEnumerationSkipsHiddenFiles | NSDirectoryEnumerationSkipsPackageDescendants)
                                         errorHandler:^BOOL(NSURL *url, NSError *error) {
            mx_log(@"FileAccess", @"enumeration error %@: %@", url.lastPathComponent, error.localizedDescription);
            return YES;
        }];
        NSUInteger visited = 0;
        for (NSURL *u in e) {
            visited++;
            if (visited > 30000 || found.count >= 160) break;
            NSNumber *isDir = nil;
            [u getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
            if (isDir.boolValue) {
                NSString *rel = [self relativePathForURL:u];
                NSUInteger depth = [[rel pathComponents] count];
                if (depth > 10) [e skipDescendants];
                continue;
            }
            if ([u.pathExtension.lowercaseString isEqualToString:@"exe"]) [found addObject:u];
        }
        mx_log(@"FileAccess", @"EXE metadata scan visited=%lu found=%lu", (unsigned long)visited, (unsigned long)found.count);
    };

    NSFileCoordinator *coord = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    [coord coordinateReadingItemAtURL:self.rootURL
                              options:NSFileCoordinatorReadingWithoutChanges
                                error:&enumError
                           byAccessor:^(NSURL *newURL) { enumerateBlock(newURL); }];
    if (enumError) {
        mx_log(@"FileAccess", @"coordinated enumeration FAILED: %@; trying direct enumeration", enumError.localizedDescription);
        [found removeAllObjects];
        enumerateBlock(self.rootURL);
    }

    [found sortUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
        NSString *an = a.lastPathComponent.lowercaseString;
        NSString *bn = b.lastPathComponent.lowercaseString;
        int ap = [an isEqualToString:@"launcher.exe"] ? 0 :
                 ([an containsString:@"tlou"] || [an containsString:@"lastofus"] ? 1 : 2);
        int bp = [bn isEqualToString:@"launcher.exe"] ? 0 :
                 ([bn containsString:@"tlou"] || [bn containsString:@"lastofus"] ? 1 : 2);
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
    [self ensureWineDriveMapping];
    mx_log(@"ExternalStorage", @"scanning EXE metadata under %@", self.rootURL.lastPathComponent);

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSArray<NSURL *> *exes = [self scanExecutablesSynchronously];
        self.lastExecutables = exes;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (exes.count == 0) {
                [self alert:@"Aucun .exe" message:@"Aucun exécutable Windows n'a été trouvé dans les 10 premiers niveaux du dossier. Aucun fichier n'a été copié."];
                return;
            }
            UIViewController *vc = [self topController];
            if (!vc) return;
            UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Choisir le .exe"
                                                                           message:[NSString stringWithFormat:@"%lu exécutable(s) trouvé(s). Les fichiers restent sur le stockage externe.", (unsigned long)exes.count]
                                                                    preferredStyle:UIAlertControllerStyleActionSheet];
            __weak typeof(self) weakSelf = self;
            NSUInteger limit = MIN((NSUInteger)30, exes.count);
            for (NSUInteger i = 0; i < limit; i++) {
                NSURL *u = exes[i];
                NSString *rel = [self relativePathForURL:u] ?: u.lastPathComponent;
                NSString *label = rel.length > 70 ? [@"…" stringByAppendingString:[rel substringFromIndex:rel.length - 69]] : rel;
                [sheet addAction:[UIAlertAction actionWithTitle:label style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
                    [weakSelf selectExecutableURL:u];
                }]];
            }
            if (exes.count > limit) {
                [sheet addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"%lu autres masqués", (unsigned long)(exes.count - limit)] style:UIAlertActionStyleDefault handler:nil]];
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
        [self alert:@"EXE invalide" message:@"L'exécutable n'est pas contenu dans le dossier externe sélectionné."];
        return;
    }
    self.selectedExeRelative = rel;
    [NSUserDefaults.standardUserDefaults setObject:rel forKey:MXExtExeRelativeKey];
    mx_log(@"ExternalStorage", @"selected EXE=%@", rel);

    NSString *win = [[NSString stringWithFormat:@"E:\\%@", rel] stringByReplacingOccurrencesOfString:@"/" withString:@"\\"];
    [self alert:@"Jeu externe prêt"
         message:[NSString stringWithFormat:@"EXE : %@\nWine : %@\n\nLe dossier reste la source principale; Madeira ne crée pas de copie du jeu.", rel, win]];
}

- (NSURL *)bestTestFile
{
    if (self.selectedExeRelative.length) {
        NSURL *selected = [self.rootURL URLByAppendingPathComponent:self.selectedExeRelative];
        NSDictionary *attrs = [NSFileManager.defaultManager attributesOfItemAtPath:selected.path error:nil];
        unsigned long long size = [attrs fileSize];
        if (size > 0) return selected;
    }

    NSDirectoryEnumerator *e = [NSFileManager.defaultManager enumeratorAtURL:self.rootURL
                                                  includingPropertiesForKeys:@[NSURLFileSizeKey, NSURLIsRegularFileKey]
                                                                     options:NSDirectoryEnumerationSkipsHiddenFiles
                                                                errorHandler:nil];
    NSURL *fallback = nil;
    NSUInteger seen = 0;
    for (NSURL *u in e) {
        if (++seen > 2500) break;
        NSNumber *regular = nil, *size = nil;
        [u getResourceValue:&regular forKey:NSURLIsRegularFileKey error:nil];
        if (!regular.boolValue) continue;
        [u getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
        unsigned long long n = size.unsignedLongLongValue;
        if (!fallback && n > 0) fallback = u;
        if (n >= (4ULL << 20) && n <= (64ULL << 20)) return u;
    }
    return fallback;
}

- (void)testStorage
{
    if (!self.rootURL || !self.scopeActive) {
        [self alert:@"Test externe" message:@"Aucun dossier externe actif."];
        return;
    }
    NSURL *root = self.rootURL;
    mx_log(@"SMB", @"starting external storage test root=%@", root.path);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        struct stat rst;
        errno = 0;
        BOOL accessible = stat(root.fileSystemRepresentation, &rst) == 0 && S_ISDIR(rst.st_mode);
        BOOL canEnumerate = NO;
        DIR *dir = opendir(root.fileSystemRepresentation);
        if (dir) { canEnumerate = YES; closedir(dir); }
        mx_log(@"FileAccess", @"root stat=%@ opendir=%@ errno=%d", accessible ? @"YES" : @"NO", canEnumerate ? @"YES" : @"NO", errno);

        NSURL *file = [self bestTestFile];
        __block BOOL readable = NO, mmapOK = NO;
        __block unsigned long long fileSize = 0, bytesRead = 0;
        __block double latencyMs = 0, throughput = 0;
        if (file) {
            NSNumber *sizeNum = nil;
            [file getResourceValue:&sizeNum forKey:NSURLFileSizeKey error:nil];
            fileSize = sizeNum.unsignedLongLongValue;
            double t0 = mx_now_seconds();
            int fd = open(file.fileSystemRepresentation, O_RDONLY);
            if (fd >= 0) {
                char first[4096];
                ssize_t firstN = read(fd, first, sizeof(first));
                double t1 = mx_now_seconds();
                latencyMs = (t1 - t0) * 1000.0;
                readable = firstN >= 0;
                if (firstN > 0) bytesRead += (unsigned long long)firstN;

                if (fileSize > 0) {
                    size_t mapLen = (size_t)MIN((unsigned long long)4096, fileSize);
                    void *p = mmap(NULL, mapLen, PROT_READ, MAP_PRIVATE, fd, 0);
                    if (p != MAP_FAILED) { volatile unsigned char x = *((unsigned char *)p); (void)x; mmapOK = YES; munmap(p, mapLen); }
                }

                lseek(fd, 0, SEEK_SET);
                const size_t chunk = 1024 * 1024;
                char *buf = malloc(chunk);
                unsigned long long maxRead = MIN(fileSize, 16ULL << 20);
                bytesRead = 0;
                double r0 = mx_now_seconds();
                while (buf && bytesRead < maxRead) {
                    size_t want = (size_t)MIN((unsigned long long)chunk, maxRead - bytesRead);
                    ssize_t n = read(fd, buf, want);
                    if (n <= 0) break;
                    bytesRead += (unsigned long long)n;
                }
                double r1 = mx_now_seconds();
                if (buf) free(buf);
                if (r1 > r0 && bytesRead > 0) throughput = ((double)bytesRead / (1024.0 * 1024.0)) / (r1 - r0);
                close(fd);
                mx_log(@"FileAccess", @"read test file=%@ size=%llu read=%llu latency=%.2fms speed=%.1fMiB/s mmap=%@",
                       file.lastPathComponent, fileSize, bytesRead, latencyMs, throughput, mmapOK ? @"YES" : @"NO");
            } else {
                mx_log(@"FileAccess", @"open test file FAILED %@ errno=%d", file.lastPathComponent, errno);
            }
        }

        BOOL writable = NO;
        NSString *tmpName = [NSString stringWithFormat:@".madeira-write-test-%u.tmp", arc4random()];
        NSURL *tmp = [root URLByAppendingPathComponent:tmpName];
        int wfd = open(tmp.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (wfd >= 0) {
            char data[4096]; memset(data, 0x4d, sizeof(data));
            ssize_t wn = write(wfd, data, sizeof(data));
            if (wn == sizeof(data)) { fsync(wfd); writable = YES; }
            close(wfd);
            unlink(tmp.fileSystemRepresentation);
        }
        mx_log(@"FileAccess", @"write test=%@", writable ? @"YES" : @"NO");

        NSString *steamapps = [root.path stringByAppendingPathComponent:@"steamapps"];
        BOOL steamLibrary = [NSFileManager.defaultManager fileExistsAtPath:steamapps];
        NSUInteger manifests = 0;
        if (steamLibrary) {
            NSArray *items = [NSFileManager.defaultManager contentsOfDirectoryAtPath:steamapps error:nil];
            for (NSString *name in items) if ([name hasPrefix:@"appmanifest_"] && [name hasSuffix:@".acf"]) manifests++;
            mx_log(@"SMB", @"Steam library detected: steamapps manifests=%lu", (unsigned long)manifests);
        }

        NSString *result = [NSString stringWithFormat:
            @"Dossier accessible : %@\nÉnumération : %@\nLecture : %@\nÉcriture : %@\nmmap : %@\nChemin : %@\nFichier test : %@\nTaille : %.2f MiB\nLecture test : %.2f MiB\nDébit séquentiel approx. : %.1f MiB/s\nLatence approx. : %.2f ms\nSteamapps : %@%@",
            accessible ? @"oui" : @"non",
            canEnumerate ? @"oui" : @"non",
            readable ? @"oui" : @"non",
            writable ? @"oui" : @"non",
            mmapOK ? @"oui" : @"non",
            root.path ?: @"<indisponible>",
            file.lastPathComponent ?: @"aucun",
            (double)fileSize / (1024.0 * 1024.0),
            (double)bytesRead / (1024.0 * 1024.0),
            throughput, latencyMs,
            steamLibrary ? @"oui" : @"non",
            steamLibrary ? [NSString stringWithFormat:@" (%lu manifestes)", (unsigned long)manifests] : @""];
        dispatch_async(dispatch_get_main_queue(), ^{ [self alert:@"Test stockage externe" message:result]; });
    });
}

- (void)launchSelectedExecutable
{
    if (!self.rootURL || !self.scopeActive || !self.selectedExeRelative.length) {
        [self alert:@"Lancement externe" message:@"Sélectionne d'abord un dossier puis un .exe."];
        return;
    }
    if (wine_process_is_running()) {
        [self alert:@"Wine déjà actif" message:@"Ferme la session Wine en cours avant un lancement direct externe."];
        return;
    }
    if (![self ensureWineDriveMapping]) return;

    NSURL *exeURL = [self.rootURL URLByAppendingPathComponent:self.selectedExeRelative];
    int fd = open(exeURL.fileSystemRepresentation, O_RDONLY);
    if (fd < 0) {
        mx_log(@"SMB", @"preflight EXE open FAILED errno=%d path=%@", errno, exeURL.path);
        [self alert:@"EXE inaccessible" message:@"Le fichier n'est pas lisible actuellement depuis le provider SMB. Lance d'abord “Tester le stockage externe” ou reconnecte le dossier."];
        return;
    }
    close(fd);

    NSString *relativeWin = [self.selectedExeRelative stringByReplacingOccurrencesOfString:@"/" withString:@"\\"];
    NSString *winExe = [NSString stringWithFormat:@"E:\\%@", relativeWin];
    NSString *relDir = [relativeWin stringByDeletingLastPathComponent];
    NSString *winDir = (relDir.length && ![relDir isEqualToString:@"."])
        ? [NSString stringWithFormat:@"E:\\%@", relDir]
        : @"E:\\";
    NSURL *hostDirURL = [exeURL URLByDeletingLastPathComponent];

    setenv("MADEIRA_EXE", winExe.UTF8String, 1);
    setenv("MADEIRA_WORKDIR", winDir.UTF8String, 1);
    setenv("MADEIRA_HOST_WORKDIR", hostDirURL.path.fileSystemRepresentation, 1);
    setenv("MADEIRA_USE_ARM64EC", "1", 1);
    setenv("MADEIRA_EXTERNAL_ROOT_E", self.rootURL.path.fileSystemRepresentation, 1);
    unsetenv("MADEIRA_ARGS");

    mx_log(@"WineDrive", @"launch EXE=%@ CWD=%@ hostCWD=%@", winExe, winDir, hostDirURL.path);
    mx_log(@"SMB", @"no Madeira-side copy requested; source remains File Provider root %@", self.rootURL.lastPathComponent);

    NSString *prefix = [self prefixPath];
    if (!wineserver_is_running()) {
        int rc = wineserver_start(prefix.fileSystemRepresentation);
        if (rc != 0) {
            [self alert:@"wineserver" message:[NSString stringWithFormat:@"Échec wineserver (code %d)", rc]];
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
            mx_log(@"WineDrive", @"wine_process_start=%d", rc);
            if (rc != 0) [weakSelf alert:@"Wine" message:[NSString stringWithFormat:@"wine_process_start a renvoyé %d", rc]];
            else [weakSelf alert:@"Lancement SMB"
                          message:[NSString stringWithFormat:@"%@\n\nLecteur : E:\\\nSource : %@\n\nLe security scope reste actif pendant la session. Madeira ne copie pas le dossier du jeu.", winExe, weakSelf.rootURL.lastPathComponent]];
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

- (void)applicationWillTerminate:(NSNotification *)n
{
    (void)n;
    if (self.scopeActive && self.rootURL) {
        [self.rootURL stopAccessingSecurityScopedResource];
        self.scopeActive = NO;
        mx_log(@"SecurityScope", @"released scope on application termination");
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
static void MXInstallExternalSMBStorage(void)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        [[MXExternalSMBStorage shared] start];
    });
}
