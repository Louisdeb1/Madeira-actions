/*
 * ExternalSMBPickerFix.inc
 *
 * Fixes the folder picker hand-off used by ExternalSMBStorage.inc.
 * The old implementation could present UIDocumentPickerViewController while
 * the action sheet was still being dismissed. On device this can leave the
 * picker visible but with "Open" failing to complete the selection callback.
 *
 * This fix:
 *  - always dismisses any UIAlertController before presenting Files
 *  - presents the picker full-screen from the real top controller
 *  - retains the picker strongly until callback/cancel
 *  - logs and forwards both modern and legacy picker callbacks
 *  - stores a real security-scoped bookmark, not a minimal bookmark
 *  - resolves bookmarks with NSURLBookmarkResolutionWithSecurityScope
 */

#import <objc/runtime.h>

static UIDocumentPickerViewController *mx_smb_active_picker = nil;

@interface MXSMBExternalStorage (PickerFix)
- (UIViewController *)topController;
- (BOOL)attachRootURL:(NSURL *)url persist:(BOOL)persist;
- (void)validateStoredExecutable;
- (void)mx_fixed_pickFolder;
- (BOOL)mx_fixed_saveBookmarkForURL:(NSURL *)url;
- (void)mx_fixed_restoreBookmarkIfPresent;
- (void)mx_fixed_documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls;
- (void)mx_fixed_documentPickerWasCancelled:(UIDocumentPickerViewController *)controller;
@end

static void mx_smb_swap_instance_method(Class cls, SEL original, SEL replacement)
{
    Method a = class_getInstanceMethod(cls, original);
    Method b = class_getInstanceMethod(cls, replacement);
    if (a && b) method_exchangeImplementations(a, b);
}

@implementation MXSMBExternalStorage (PickerFix)

+ (void)load
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Class cls = [MXSMBExternalStorage class];
        mx_smb_swap_instance_method(cls, @selector(pickFolder), @selector(mx_fixed_pickFolder));
        mx_smb_swap_instance_method(cls, @selector(saveBookmarkForURL:), @selector(mx_fixed_saveBookmarkForURL:));
        mx_smb_swap_instance_method(cls, @selector(restoreBookmarkIfPresent), @selector(mx_fixed_restoreBookmarkIfPresent));
        mx_smb_swap_instance_method(cls,
                                    @selector(documentPicker:didPickDocumentsAtURLs:),
                                    @selector(mx_fixed_documentPicker:didPickDocumentsAtURLs:));
        mx_smb_swap_instance_method(cls,
                                    @selector(documentPickerWasCancelled:),
                                    @selector(mx_fixed_documentPickerWasCancelled:));
        mx_smb_log(@"ExternalStorage", @"picker presentation/bookmark fix installed");
    });
}

- (void)mx_fixed_pickFolder
{
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *current = [self topController];

        void (^presentPicker)(void) = ^{
            UIViewController *presenter = [self topController];
            if (!presenter) {
                mx_smb_log(@"ExternalStorage", @"picker FIX: no presenter");
                [self alert:@"Sélecteur indisponible" message:@"Impossible d'ouvrir Fichiers. Réessaie dans une seconde."];
                return;
            }

            UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
                                                       initForOpeningContentTypes:@[UTTypeFolder]
                                                       asCopy:NO];
            picker.delegate = self;
            picker.allowsMultipleSelection = NO;
            picker.modalPresentationStyle = UIModalPresentationFullScreen;
            if (self.rootURL) picker.directoryURL = self.rootURL;
            mx_smb_active_picker = picker;

            mx_smb_log(@"ExternalStorage", @"picker FIX: presenting from %@", NSStringFromClass(presenter.class));
            [presenter presentViewController:picker animated:YES completion:^{
                mx_smb_log(@"ExternalStorage", @"picker FIX: presentation complete");
            }];
        };

        if ([current isKindOfClass:UIAlertController.class]) {
            mx_smb_log(@"ExternalStorage", @"picker FIX: dismissing action sheet before Files");
            [current dismissViewControllerAnimated:YES completion:^{
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.18 * NSEC_PER_SEC)),
                               dispatch_get_main_queue(), presentPicker);
            }];
        } else {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.08 * NSEC_PER_SEC)),
                           dispatch_get_main_queue(), presentPicker);
        }
    });
}

- (void)mx_fixed_documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
    mx_smb_log(@"ExternalStorage", @"picker FIX: modern callback urls=%lu", (unsigned long)urls.count);
    mx_smb_active_picker = nil;
    [self mx_fixed_documentPicker:controller didPickDocumentsAtURLs:urls];
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-implementations"
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentAtURL:(NSURL *)url
{
    mx_smb_log(@"ExternalStorage", @"picker FIX: legacy callback url=%@", url.path ?: @"<nil>");
    mx_smb_active_picker = nil;
    if (url) [self documentPicker:controller didPickDocumentsAtURLs:@[url]];
}
#pragma clang diagnostic pop

- (void)mx_fixed_documentPickerWasCancelled:(UIDocumentPickerViewController *)controller
{
    mx_smb_log(@"ExternalStorage", @"picker FIX: cancelled");
    mx_smb_active_picker = nil;
    [self mx_fixed_documentPickerWasCancelled:controller];
}

- (BOOL)mx_fixed_saveBookmarkForURL:(NSURL *)url
{
    NSError *error = nil;
    NSData *bookmark = [url bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope
                         includingResourceValuesForKeys:nil
                                          relativeToURL:nil
                                                  error:&error];

    if (!bookmark) {
        mx_smb_log(@"SecurityScope", @"security-scoped bookmark FAILED (%@); retrying implicit scope",
                   error.localizedDescription ?: @"unknown");
        error = nil;
        bookmark = [url bookmarkDataWithOptions:0
                     includingResourceValuesForKeys:nil
                                      relativeToURL:nil
                                              error:&error];
    }

    if (!bookmark) {
        mx_smb_log(@"SecurityScope", @"bookmark save FAILED: %@", error.localizedDescription ?: @"unknown");
        return NO;
    }

    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    [d setObject:bookmark forKey:MXSMBBookmarkKey];
    [d setObject:url.lastPathComponent ?: @"External" forKey:MXSMBNameKey];
    [d synchronize];
    mx_smb_log(@"SecurityScope", @"bookmark saved with security scope bytes=%lu", (unsigned long)bookmark.length);
    return YES;
}

- (void)mx_fixed_restoreBookmarkIfPresent
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
    NSURLBookmarkResolutionOptions options = NSURLBookmarkResolutionWithSecurityScope | NSURLBookmarkResolutionWithoutUI;
    NSURL *url = [NSURL URLByResolvingBookmarkData:bookmark
                                           options:options
                                     relativeToURL:nil
                               bookmarkDataIsStale:&stale
                                             error:&error];

    if (!url) {
        mx_smb_log(@"SecurityScope", @"security-scope resolution failed (%@); retrying legacy bookmark",
                   error.localizedDescription ?: @"unknown");
        error = nil;
        stale = NO;
        url = [NSURL URLByResolvingBookmarkData:bookmark
                                        options:NSURLBookmarkResolutionWithoutUI
                                  relativeToURL:nil
                            bookmarkDataIsStale:&stale
                                          error:&error];
    }

    if (!url) {
        mx_smb_log(@"SecurityScope", @"bookmark resolve FAILED: %@", error.localizedDescription ?: @"unknown");
        return;
    }

    mx_smb_log(@"SecurityScope", @"bookmark FIX resolved stale=%@ path=%@",
               stale ? @"YES" : @"NO", url.path ?: @"<nil>");

    if ([self attachRootURL:url persist:stale]) {
        if (stale) mx_smb_log(@"SecurityScope", @"stale bookmark recreated with security scope");
        [self validateStoredExecutable];
    }
}

@end
