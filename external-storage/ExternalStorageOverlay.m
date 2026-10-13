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
@property(nonatomic,strong) NSURL *scopedEXE;
@property(nonatomic,strong) NSURL *scopedRoot;
@property(nonatomic,assign) BOOL exeScopeStarted;
@property(nonatomic,assign) BOOL rootScopeStarted;
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
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return UIApplication.sharedApplication.windows.firstObject;
#pragma clang diagnostic pop
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

- (void)keepButtonFront:(NSInteger)attempt {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIWindow *w = [self activeWindow];
        if (w && self.button.superview == w) [w bringSubviewToFront:self.button];
        if (attempt < 20) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                [self keepButtonFront:attempt + 1];
            });
        }
    });
}

- (void)installButtonWhenReady:(NSInteger)attempt {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIWindow *w = [self activeWindow];
        if (!w) {
            if (attempt < 40) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [self installButtonWhenReady:attempt + 1]; });
            return;
        }
        if (self.button.superview) { [w bringSubviewToFront:self.button]; return; }

        UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
        b.frame = CGRectMake(MAX(12.0, w.bounds.size.width - 108.0), w.safeAreaInsets.top + 8.0, 96.0, 44.0);
        b.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleBottomMargin;
        [b setTitle:@"EXE NATIF" forState:UIControlStateNormal];
        b.titleLabel.font = [UIFont boldSystemFontOfSize:12.0];
        b.backgroundColor = [UIColor colorWithRed:0.08 green:0.42 blue:0.95 alpha:0.96];
        [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
        b.layer.cornerRadius = 14.0;
        b.layer.borderWidth = 1.0;
        b.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.32].CGColor;
        b.accessibilityLabel = @"Choisir un EXE Windows avec le type natif Apple";
        [b addTarget:self action:@selector(pickExecutable) forControlEvents:UIControlEventTouchUpInside];
        [w addSubview:b];
        [w bringSubviewToFront:b];
        self.button = b;
        [self keepButtonFront:0];
    });
}

- (void)pickExecutable {
    if (wine_process_is_running()) {
        [self alert:@"Madeira" message:@"Un jeu est déjà en cours. Ferme-le avant d'en lancer un autre."];
        return;
    }
    UIViewController *vc = [self topController];
    if (!vc) return;

    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeEXE] asCopy:NO];
    picker.delegate = self;
    // iOS affiche alors une selection + le bouton Ouvrir au lieu de fermer immediatement sur un tap.
    picker.allowsMultipleSelection = YES;
    if ([picker respondsToSelector:@selector(setShouldShowFileExtensions:)]) picker.shouldShowFileExtensions = YES;
    [vc presentViewController:picker animated:YES completion:nil];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *exe = nil;
    for (NSURL *candidate in urls) {
        if ([candidate.pathExtension.lowercaseString isEqualToString:@"exe"]) { exe = candidate; break; }
    }
    if (!exe) {
        [self alert:@"Pas un EXE" message:@"Selectionne un fichier Windows .exe, puis touche Ouvrir."];
        return;
    }

    if (self.exeScopeStarted && self.scopedEXE) [self.scopedEXE stopAccessingSecurityScopedResource];
    if (self.rootScopeStarted && self.scopedRoot) [self.scopedRoot stopAccessingSecurityScopedResource];
    self.scopedEXE = exe;
    self.scopedRoot = nil;
    self.exeScopeStarted = [exe startAccessingSecurityScopedResource];
    self.rootScopeStarted = NO;

    [self offerAccessibleRootsForExecutable:exe];
}

- (BOOL)directoryReadable:(NSURL *)dir {
    if (!dir || !dir.isFileURL) return NO;
    NSNumber *isDir = nil;
    NSError *metaError = nil;
    if (![dir getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:&metaError] || !isDir.boolValue) return NO;

    __block BOOL ok = NO;
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSError *coordError = nil;
    [coordinator coordinateReadingItemAtURL:dir options:0 error:&coordError byAccessor:^(NSURL *newURL) {
        NSError *e = nil;
        NSArray *items = [NSFileManager.defaultManager contentsOfDirectoryAtURL:newURL
                                                    includingPropertiesForKeys:@[NSURLNameKey]
                                                                       options:NSDirectoryEnumerationSkipsHiddenFiles
                                                                         error:&e];
        ok = (items != nil);
    }];
    return ok && coordError == nil;
}

- (NSString *)shortPathForURL:(NSURL *)url {
    NSArray<NSString *> *c = url.pathComponents;
    if (c.count <= 3) return url.path;
    NSUInteger start = c.count - MIN((NSUInteger)3, c.count);
    return [[c subarrayWithRange:NSMakeRange(start, c.count - start)] componentsJoinedByString:@"/"];
}

- (void)offerAccessibleRootsForExecutable:(NSURL *)exe {
    NSMutableArray<NSURL *> *roots = [NSMutableArray array];
    NSURL *dir = exe.URLByDeletingLastPathComponent;

    for (NSInteger i = 0; i < 5 && dir; i++) {
        BOOL started = [dir startAccessingSecurityScopedResource];
        BOOL readable = [self directoryReadable:dir];
        if (started) [dir stopAccessingSecurityScopedResource];
        if (readable) [roots addObject:dir];

        NSURL *parent = dir.URLByDeletingLastPathComponent;
        if (!parent || [parent.path isEqualToString:dir.path] || [parent.path isEqualToString:@"/"]) break;
        dir = parent;
    }

    if (roots.count == 0) {
        [self alert:@"EXE ouvert, dossier bloque"
            message:@"Le .exe a bien ete transmis a Madeira, mais iOS n'accorde pas l'acces a ses fichiers voisins. Il faut alors passer a un vrai montage de dossier plutot qu'au fichier seul."];
        return;
    }

    UIViewController *vc = [self topController];
    if (!vc) return;

    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Dossier racine du jeu"
                                                                   message:@"Choisis le dossier qui contient tout le jeu. Pour Unreal, prends normalement le dossier AU-DESSUS de Binaries."
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    __weak typeof(self) weakSelf = self;
    for (NSURL *root in roots) {
        NSString *label = [self shortPathForURL:root];
        [sheet addAction:[UIAlertAction actionWithTitle:label style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
            [weakSelf launchExecutable:exe root:root];
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Annuler" style:UIAlertActionStyleCancel handler:nil]];
    UIPopoverPresentationController *pop = sheet.popoverPresentationController;
    if (pop) {
        pop.sourceView = self.button ?: vc.view;
        pop.sourceRect = self.button ? self.button.bounds : CGRectMake(vc.view.bounds.size.width - 1, 1, 1, 1);
    }
    [vc presentViewController:sheet animated:YES completion:nil];
}

- (void)launchExecutable:(NSURL *)exe root:(NSURL *)root {
    NSString *exePath = exe.standardizedURL.path;
    NSString *rootPath = root.standardizedURL.path;
    NSString *prefixWithSlash = [rootPath hasSuffix:@"/"] ? rootPath : [rootPath stringByAppendingString:@"/"];
    if (![exePath hasPrefix:prefixWithSlash]) {
        [self alert:@"Chemin invalide" message:@"Le .exe n'est pas contenu dans le dossier choisi."];
        return;
    }

    if (self.rootScopeStarted && self.scopedRoot) [self.scopedRoot stopAccessingSecurityScopedResource];
    self.scopedRoot = root;
    self.rootScopeStarted = [root startAccessingSecurityScopedResource];

    if (![self directoryReadable:root]) {
        [self alert:@"Acces perdu" message:@"iOS ne permet plus de lire le dossier choisi. Le jeu ne pourrait pas charger ses DLL et DATA."];
        return;
    }

    NSString *relative = [exePath substringFromIndex:prefixWithSlash.length];
    if (!relative.length || ![relative canBeConvertedToEncoding:NSASCIIStringEncoding]) {
        [self alert:@"Chemin EXE" message:@"Le chemin relatif du .exe doit utiliser des caracteres ASCII pour cette build."];
        return;
    }
    NSString *winRelative = [relative stringByReplacingOccurrencesOfString:@"/" withString:@"\\"];

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
    if (![fm createSymbolicLinkAtPath:link withDestinationPath:rootPath error:&err]) {
        [self alert:@"Montage SSD impossible" message:err.localizedDescription ?: @"Impossible de creer le lien vers le dossier du jeu."];
        return;
    }

    NSString *winExe = [NSString stringWithFormat:@"C:\\__ExternalGame\\%@", winRelative];
    NSString *relDir = [winRelative stringByDeletingLastPathComponent];
    NSString *workDir = relDir.length && ![relDir isEqualToString:@"."]
        ? [NSString stringWithFormat:@"C:\\__ExternalGame\\%@", relDir]
        : @"C:\\__ExternalGame";

    setenv("MADEIRA_EXE", winExe.UTF8String, 1);
    setenv("MADEIRA_WORKDIR", workDir.UTF8String, 1);
    setenv("MADEIRA_EXTERNAL_ROOT", rootPath.fileSystemRepresentation, 1);
    unsetenv("MADEIRA_ARGS");

    int ws = wineserver_is_running();
    if (!ws) {
        int rc = wineserver_start(prefix.fileSystemRepresentation);
        if (rc != 0) { [self alert:@"wineserver" message:[NSString stringWithFormat:@"Impossible de demarrer wineserver (code %d).", rc]]; return; }
    }

    __block NSInteger tries = 0;
    __weak typeof(self) weakSelf = self;
    __block void (^poll)(void) = nil;
    poll = ^{
        tries++;
        if (wineserver_is_running()) {
            int rc = wine_process_start(prefix.fileSystemRepresentation);
            if (rc != 0) {
                [weakSelf alert:@"Lancement impossible" message:[NSString stringWithFormat:@"wine_process_start a renvoye %d.", rc]];
            } else {
                [weakSelf alert:@"Lancement externe"
                         message:[NSString stringWithFormat:@"%@\n\nRacine montee : %@\n\nLe dossier reste ouvert pour Wine pendant le jeu.", exe.lastPathComponent, root.lastPathComponent]];
            }
            poll = nil;
            return;
        }
        if (tries >= 100) {
            [weakSelf alert:@"wineserver" message:@"wineserver ne s'est pas declare pret."];
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
