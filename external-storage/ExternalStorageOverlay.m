@import UIKit;
@import Foundation;
@import UniformTypeIdentifiers;
@import QuartzCore;
#include <stdlib.h>
#include <unistd.h>

extern int wineserver_start(const char *prefix_path);
extern int wineserver_is_running(void);
extern void wineserver_stop(void);
extern int wine_process_start(const char *prefix_path);
extern int wine_process_is_running(void);

@interface MXExternalLauncher : NSObject <UIDocumentPickerDelegate>
@property(nonatomic,strong) UIButton *button;
@property(nonatomic,strong) NSURL *scopedURL;
@property(nonatomic,assign) BOOL scopeStarted;
@end

@implementation MXExternalLauncher

+ (instancetype)shared {
    static MXExternalLauncher *obj;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ obj = [MXExternalLauncher new]; });
    return obj;
}

- (UIWindow *)activeWindow {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (scene.activationState != UISceneActivationStateForegroundActive) continue;
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        UIWindowScene *ws = (UIWindowScene *)scene;
        for (UIWindow *w in ws.windows) if (w.isKeyWindow) return w;
        if (ws.windows.firstObject) return ws.windows.firstObject;
    }
    return UIApplication.sharedApplication.windows.firstObject;
}

- (UIViewController *)topController {
    UIViewController *vc = [self activeWindow].rootViewController;
    while (vc.presentedViewController) vc = vc.presentedViewController;
    if ([vc isKindOfClass:UINavigationController.class]) vc = ((UINavigationController *)vc).topViewController;
    if ([vc isKindOfClass:UITabBarController.class]) vc = ((UITabBarController *)vc).selectedViewController;
    return vc;
}

- (void)alert:(NSString *)title message:(NSString *)message {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *vc = [self topController];
        if (!vc) return;
        UIAlertController *a = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [vc presentViewController:a animated:YES completion:nil];
    });
}

- (void)installButtonWhenReady:(NSInteger)attempt {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIWindow *w = [self activeWindow];
        if (!w) {
            if (attempt < 20) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [self installButtonWhenReady:attempt + 1]; });
            return;
        }
        if (self.button.superview) return;
        UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
        b.frame = CGRectMake(MAX(12.0, w.bounds.size.width - 82.0), w.safeAreaInsets.top + 12.0, 68.0, 42.0);
        b.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleBottomMargin;
        [b setTitle:@"USB / SSD" forState:UIControlStateNormal];
        b.titleLabel.font = [UIFont boldSystemFontOfSize:12.0];
        b.backgroundColor = [UIColor colorWithWhite:0.08 alpha:0.82];
        [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
        b.layer.cornerRadius = 14.0;
        b.layer.borderWidth = 1.0;
        b.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.22].CGColor;
        b.accessibilityLabel = @"Lancer un jeu depuis USB ou SSD";
        [b addTarget:self action:@selector(pickFolder) forControlEvents:UIControlEventTouchUpInside];
        [w addSubview:b];
        [w bringSubviewToFront:b];
        self.button = b;
    });
}

- (void)pickFolder {
    if (wine_process_is_running()) {
        [self alert:@"Madeira" message:@"Un jeu est déjà en cours. Ferme-le avant d'en lancer un autre depuis le stockage externe."];
        return;
    }
    UIViewController *vc = [self topController];
    if (!vc) return;
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[[UTType folder]] asCopy:NO];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    [vc presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *root = urls.firstObject;
    if (!root) return;
    if (self.scopeStarted && self.scopedURL) [self.scopedURL stopAccessingSecurityScopedResource];
    self.scopedURL = root;
    self.scopeStarted = [root startAccessingSecurityScopedResource];
    [self chooseExecutableUnder:root];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {}

- (void)chooseExecutableUnder:(NSURL *)root {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSArray *keys = @[NSURLIsDirectoryKey, NSURLNameKey];
    NSDirectoryEnumerator<NSURL *> *en = [fm enumeratorAtURL:root includingPropertiesForKeys:keys options:NSDirectoryEnumerationSkipsHiddenFiles errorHandler:^BOOL(NSURL *url, NSError *error) { return YES; }];
    NSMutableArray<NSURL *> *exes = [NSMutableArray array];
    NSUInteger rootDepth = root.pathComponents.count;
    for (NSURL *u in en) {
        NSNumber *isDir = nil;
        [u getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
        NSUInteger depth = u.pathComponents.count - rootDepth;
        if (isDir.boolValue && depth >= 4) { [en skipDescendants]; continue; }
        if (!isDir.boolValue && [u.pathExtension.lowercaseString isEqualToString:@"exe"]) {
            [exes addObject:u];
            if (exes.count >= 50) break;
        }
    }
    if (!exes.count) {
        [self alert:@"Aucun .exe" message:@"Je n'ai trouvé aucun fichier .exe dans ce dossier (recherche jusqu'à 4 niveaux)."];
        return;
    }
    [exes sortUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) { return [a.lastPathComponent localizedCaseInsensitiveCompare:b.lastPathComponent]; }];
    UIViewController *vc = [self topController];
    if (!vc) return;
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Choisir l'EXE" message:@"Le jeu restera sur ton USB/SSD. Madeira utilisera un lien symbolique dans son disque C:." preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSURL *exe in exes) {
        NSString *relative = exe.path;
        if ([relative hasPrefix:root.path]) relative = [relative substringFromIndex:MIN(relative.length, root.path.length + 1)];
        NSString *label = relative.length ? relative : exe.lastPathComponent;
        [sheet addAction:[UIAlertAction actionWithTitle:label style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) { [self launchExecutable:exe]; }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Annuler" style:UIAlertActionStyleCancel handler:nil]];
    UIPopoverPresentationController *pop = sheet.popoverPresentationController;
    if (pop) { pop.sourceView = self.button ?: vc.view; pop.sourceRect = self.button ? self.button.bounds : CGRectMake(vc.view.bounds.size.width - 1, 1, 1, 1); }
    [vc presentViewController:sheet animated:YES completion:nil];
}

- (void)launchExecutable:(NSURL *)exe {
    NSString *filename = exe.lastPathComponent;
    if (![filename canBeConvertedToEncoding:NSASCIIStringEncoding]) {
        [self alert:@"Nom de fichier" message:@"Pour cette version, le nom du .exe doit utiliser uniquement des caractères ASCII. Renomme uniquement le fichier .exe puis réessaie."];
        return;
    }
    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    if (!documents.length) { [self alert:@"Erreur" message:@"Dossier Documents introuvable."]; return; }
    NSString *prefix = [documents stringByAppendingPathComponent:@"wine"];
    NSString *driveC = [prefix stringByAppendingPathComponent:@"drive_c"];
    NSString *link = [driveC stringByAppendingPathComponent:@"__ExternalGame"];
    NSFileManager *fm = NSFileManager.defaultManager;
    NSError *err = nil;
    [fm createDirectoryAtPath:driveC withIntermediateDirectories:YES attributes:nil error:&err];
    if (err) { [self alert:@"Erreur" message:err.localizedDescription]; return; }
    [fm removeItemAtPath:link error:nil];
    err = nil;
    NSString *hostDir = exe.URLByDeletingLastPathComponent.path;
    if (![fm createSymbolicLinkAtPath:link withDestinationPath:hostDir error:&err]) {
        [self alert:@"Lien USB/SSD impossible" message:err.localizedDescription ?: @"Impossible de créer le lien symbolique."];
        return;
    }

    NSString *winExe = [NSString stringWithFormat:@"C:\\__ExternalGame\\%@", filename];
    setenv("MADEIRA_EXE", winExe.UTF8String, 1);
    setenv("MADEIRA_WORKDIR", "C:\\__ExternalGame", 1);
    unsetenv("MADEIRA_ARGS");

    if (wine_process_is_running()) {
        [self alert:@"Madeira" message:@"Un jeu est déjà en cours."];
        return;
    }

    int ws = wineserver_is_running();
    if (!ws) {
        int rc = wineserver_start(prefix.fileSystemRepresentation);
        if (rc != 0) { [self alert:@"wineserver" message:[NSString stringWithFormat:@"Impossible de démarrer wineserver (code %d).", rc]]; return; }
    }

    __block NSInteger tries = 0;
    __weak typeof(self) weakSelf = self;
    __block void (^poll)(void) = nil;
    poll = ^{
        tries++;
        if (wineserver_is_running()) {
            int rc = wine_process_start(prefix.fileSystemRepresentation);
            if (rc != 0) [weakSelf alert:@"Lancement impossible" message:[NSString stringWithFormat:@"wine_process_start a renvoyé %d.", rc]];
            else [weakSelf alert:@"Lancement" message:[NSString stringWithFormat:@"%@ est lancé directement depuis le stockage externe.", filename]];
            poll = nil;
            return;
        }
        if (tries >= 80) {
            [weakSelf alert:@"wineserver" message:@"wineserver ne s'est pas déclaré prêt."];
            poll = nil;
            return;
        }
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.10 * NSEC_PER_SEC)), dispatch_get_main_queue(), poll);
    };
    dispatch_async(dispatch_get_main_queue(), poll);
}
@end

__attribute__((constructor))
static void MXInstallExternalStorageOverlay(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [[MXExternalLauncher shared] installButtonWhenReady:0];
    });
}
