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
@property(nonatomic,strong) NSURL *scopedRoot;
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
        b.frame = CGRectMake(MAX(12.0, w.bounds.size.width - 118.0), w.safeAreaInsets.top + 8.0, 106.0, 44.0);
        b.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleBottomMargin;
        [b setTitle:@"DOSSIER JEU" forState:UIControlStateNormal];
        b.titleLabel.font = [UIFont boldSystemFontOfSize:11.5];
        b.backgroundColor = [UIColor colorWithRed:0.08 green:0.42 blue:0.95 alpha:0.96];
        [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
        b.layer.cornerRadius = 14.0;
        b.layer.borderWidth = 1.0;
        b.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.32].CGColor;
        b.accessibilityLabel = @"Autoriser le dossier complet du jeu sur USB ou SSD";
        [b addTarget:self action:@selector(pickGameFolder) forControlEvents:UIControlEventTouchUpInside];
        [w addSubview:b];
        [w bringSubviewToFront:b];
        self.button = b;
        [self keepButtonFront:0];
    });
}

- (void)pickGameFolder {
    if (wine_process_is_running()) {
        [self alert:@"Madeira" message:@"Un jeu est déjà en cours. Ferme-le avant d'en lancer un autre."];
        return;
    }
    UIViewController *vc = [self topController];
    if (!vc) return;

    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];
    picker.delegate = self;
    picker.allowsMultipleSelection = YES;
    [vc presentViewController:picker animated:YES completion:nil];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSArray<NSURL *> *picked = [urls copy];
    __weak typeof(self) weakSelf = self;

    [controller dismissViewControllerAnimated:YES completion:^{
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.30 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [weakSelf acceptPickedFolders:picked];
        });
    }];
}

- (void)acceptPickedFolders:(NSArray<NSURL *> *)urls {
    if (urls.count != 1) {
        [self alert:@"Un seul dossier" message:@"Sélectionne uniquement le dossier racine du jeu, puis touche Ouvrir."];
        return;
    }

    NSURL *root = urls.firstObject;
    NSNumber *isDir = nil;
    if (!root.isFileURL || ![root getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil] || !isDir.boolValue) {
        [self alert:@"Dossier invalide" message:@"Il faut sélectionner le dossier complet du jeu, pas un fichier .exe."];
        return;
    }

    if (self.rootScopeStarted && self.scopedRoot) {
        [self.scopedRoot stopAccessingSecurityScopedResource];
    }
    self.scopedRoot = root;
    self.rootScopeStarted = [root startAccessingSecurityScopedResource];

    if (!self.rootScopeStarted) {
        [self alert:@"Autorisation refusée" message:@"iOS n'a pas accordé l'accès security-scoped au dossier. Madeira ne pourrait pas lire les DLL/DATA du jeu."];
        return;
    }

    [self scanRootAndChooseExecutable:root];
}

- (void)scanRootAndChooseExecutable:(NSURL *)root {
    NSMutableArray<NSURL *> *executables = [NSMutableArray array];
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    __block NSError *scanError = nil;
    NSError *coordError = nil;

    [coordinator coordinateReadingItemAtURL:root options:0 error:&coordError byAccessor:^(NSURL *newRoot) {
        NSFileManager *fm = NSFileManager.defaultManager;
        NSArray *keys = @[NSURLIsDirectoryKey, NSURLNameKey];
        NSDirectoryEnumerator<NSURL *> *en = [fm enumeratorAtURL:newRoot
                                      includingPropertiesForKeys:keys
                                                         options:NSDirectoryEnumerationSkipsHiddenFiles
                                                    errorHandler:^BOOL(__unused NSURL *url, NSError *error) {
            scanError = error;
            return YES;
        }];

        NSString *base = newRoot.standardizedURL.path;
        for (NSURL *u in en) {
            if (executables.count >= 120) break;

            NSString *path = u.standardizedURL.path;
            NSString *relative = [path hasPrefix:base] ? [path substringFromIndex:base.length] : path;
            NSUInteger depth = relative.pathComponents.count;

            NSNumber *isDirectory = nil;
            [u getResourceValue:&isDirectory forKey:NSURLIsDirectoryKey error:nil];
            if (isDirectory.boolValue && depth > 10) {
                [en skipDescendants];
                continue;
            }
            if (!isDirectory.boolValue && [u.pathExtension.lowercaseString isEqualToString:@"exe"]) {
                [executables addObject:u];
            }
        }
    }];

    if (coordError && executables.count == 0) {
        [self alert:@"Lecture du dossier impossible" message:coordError.localizedDescription ?: @"NSFileCoordinator a refusé la lecture du dossier externe."];
        return;
    }
    if (executables.count == 0) {
        NSString *msg = scanError ? [NSString stringWithFormat:@"Aucun .exe trouvé. Dernière erreur : %@", scanError.localizedDescription] : @"Aucun fichier .exe n'a été trouvé dans ce dossier ou ses sous-dossiers.";
        [self alert:@"Aucun EXE" message:msg];
        return;
    }

    NSInteger (^score)(NSURL *) = ^NSInteger(NSURL *u) {
        NSString *n = u.lastPathComponent.lowercaseString;
        NSInteger s = 0;
        if ([n isEqualToString:@"launcher.exe"]) s += 1000;
        if ([n containsString:@"launcher"]) s += 500;
        if ([n containsString:@"tlou"] || [n containsString:@"lastofus"] || [n containsString:@"last_of_us"]) s += 400;
        if ([n containsString:@"game"]) s += 200;
        if ([n containsString:@"crash"] || [n containsString:@"report"] || [n containsString:@"unins"] || [n containsString:@"setup"]) s -= 300;
        return s;
    };

    [executables sortUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
        NSInteger sa = score(a), sb = score(b);
        if (sa != sb) return sa > sb ? NSOrderedAscending : NSOrderedDescending;
        return [a.path localizedCaseInsensitiveCompare:b.path];
    }];

    UIViewController *vc = [self topController];
    if (!vc) return;

    NSString *message = [NSString stringWithFormat:@"Dossier autorisé récursivement. %lu EXE trouvé%@. Choisis celui à lancer.", (unsigned long)executables.count, executables.count > 1 ? @"s" : @""];
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"EXE du jeu"
                                                                   message:message
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    __weak typeof(self) weakSelf = self;
    NSUInteger shown = MIN((NSUInteger)40, executables.count);
    NSString *rootPath = root.standardizedURL.path;
    NSString *prefix = [rootPath hasSuffix:@"/"] ? rootPath : [rootPath stringByAppendingString:@"/"];

    for (NSUInteger i = 0; i < shown; i++) {
        NSURL *exe = executables[i];
        NSString *ep = exe.standardizedURL.path;
        NSString *relative = [ep hasPrefix:prefix] ? [ep substringFromIndex:prefix.length] : exe.lastPathComponent;
        NSString *title = relative.length > 80 ? [NSString stringWithFormat:@"…%@", [relative substringFromIndex:relative.length - 79]] : relative;
        [sheet addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
            [weakSelf launchExecutable:exe root:root];
        }]];
    }
    if (executables.count > shown) {
        [sheet addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"%lu autres EXE non affichés", (unsigned long)(executables.count - shown)] style:UIAlertActionStyleDefault handler:nil]];
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
    if (!self.rootScopeStarted || ![self.scopedRoot.path isEqualToString:root.path]) {
        [self alert:@"Accès dossier perdu" message:@"L'autorisation du dossier n'est plus active. Resélectionne le dossier du jeu."];
        return;
    }

    NSString *exePath = exe.standardizedURL.path;
    NSString *rootPath = root.standardizedURL.path;
    NSString *prefixWithSlash = [rootPath hasSuffix:@"/"] ? rootPath : [rootPath stringByAppendingString:@"/"];
    if (![exePath hasPrefix:prefixWithSlash]) {
        [self alert:@"Chemin invalide" message:@"Le .exe choisi n'est pas contenu dans le dossier autorisé."];
        return;
    }

    NSString *relative = [exePath substringFromIndex:prefixWithSlash.length];
    if (!relative.length || ![relative canBeConvertedToEncoding:NSASCIIStringEncoding]) {
        [self alert:@"Chemin EXE" message:@"Le chemin relatif du .exe doit utiliser des caractères ASCII pour cette build."];
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
        [self alert:@"Montage SSD impossible" message:err.localizedDescription ?: @"Impossible de créer le lien vers le dossier du jeu."];
        return;
    }

    NSString *mountedEXE = [link stringByAppendingPathComponent:relative];
    if (![fm fileExistsAtPath:mountedEXE]) {
        [self alert:@"Test du montage échoué" message:@"Madeira a l'autorisation du dossier mais le .exe n'est pas visible à travers le montage __ExternalGame."];
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
        if (rc != 0) { [self alert:@"wineserver" message:[NSString stringWithFormat:@"Impossible de démarrer wineserver (code %d).", rc]]; return; }
    }

    __block NSInteger tries = 0;
    __weak typeof(self) weakSelf = self;
    __block void (^poll)(void) = nil;
    poll = ^{
        tries++;
        if (wineserver_is_running()) {
            int rc = wine_process_start(prefix.fileSystemRepresentation);
            if (rc != 0) {
                [weakSelf alert:@"Lancement impossible" message:[NSString stringWithFormat:@"wine_process_start a renvoyé %d.", rc]];
            } else {
                [weakSelf alert:@"Lancement externe"
                         message:[NSString stringWithFormat:@"%@\n\nDossier complet autorisé : %@\n\nWine garde l'accès récursif pendant la session.", exe.lastPathComponent, root.lastPathComponent]];
            }
            poll = nil;
            return;
        }
        if (tries >= 100) {
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
