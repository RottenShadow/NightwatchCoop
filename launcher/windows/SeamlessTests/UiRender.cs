using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using BloodborneLauncher.Views;

internal static class UiRender
{
    public static void Check(string directory)
    {
        Exception? failure = null;
        var thread = new Thread(() =>
        {
            try
            {
                Directory.CreateDirectory(directory);
                var app = new BloodborneLauncher.App();
                app.InitializeComponent();
                // Each mode shows its own rows: Host and Join their party rows, single player only the saves,
                // Steam P2P the account rows instead of the direct-only Host/Join and party name rows.
                foreach (var (mode, name) in new[] { ("host", "host"), ("join", "join"), ("", "single"), ("steam", "steam") })
                {
                    var steam = mode == "steam";
                    var settings = BloodborneLauncher.Core.Workspace.Current.Settings;
                    settings.Set("party_mode", steam ? "" : mode);
                    settings.Set("party_transport", steam ? "steam" : "direct");
                    if (steam)
                    {
                        settings.Set("party_name", "Join_Test");
                        settings.Set("steam_appid", "480");
                        settings.PartyPassword = "steam secret";
                    }
                    var page = new SeamlessPage();
                    page.RaiseEvent(new RoutedEventArgs(FrameworkElement.LoadedEvent));
                    var scroll = (ScrollViewer)page.Content;
                    page.Content = null;
                    var surface = new Border { Background = new SolidColorBrush(Color.FromRgb(24, 24, 28)), Child = scroll };
                    surface.SetResourceReference(System.Windows.Documents.TextElement.ForegroundProperty, "TextFillColorPrimaryBrush");
                    surface.Measure(new Size(920, 760));
                    surface.Arrange(new Rect(0, 0, 920, 760));
                    surface.UpdateLayout();
                    if (steam)
                    {
                        var nameRow = Descendants(surface).OfType<SettingRow>().FirstOrDefault(row => row.Title == "Your name")
                            ?? throw new InvalidOperationException("The party name row is missing.");
                        if (nameRow.Visibility == Visibility.Visible)
                            throw new InvalidOperationException("The direct-only party name row is visible in Steam mode.");
                        var modeRow = Descendants(surface).OfType<SettingRow>().FirstOrDefault(row => row.Title == "Mode")
                            ?? throw new InvalidOperationException("The party mode row is missing.");
                        if (modeRow.Visibility == Visibility.Visible)
                            throw new InvalidOperationException("The Host/Join mode row is visible in Steam mode.");
                    }
                    if ((mode.Length > 0 || steam) && scroll.ExtentHeight <= scroll.ViewportHeight) throw new InvalidOperationException("Party settings did not lay out.");
                    foreach (var (label, fraction) in new[] { ("party", 0.0), ("rules", 0.5), ("saves", 1.0) })
                    {
                        scroll.ScrollToVerticalOffset(scroll.ScrollableHeight * fraction);
                        surface.UpdateLayout();
                        var image = new RenderTargetBitmap(920, 760, 96, 96, PixelFormats.Pbgra32);
                        image.Render(surface);
                        var encoder = new PngBitmapEncoder();
                        encoder.Frames.Add(BitmapFrame.Create(image));
                        using var output = File.Create(Path.Combine(directory, $"seamless-{name}-{label}.png"));
                        encoder.Save(output);
                    }
                }
                BloodborneLauncher.Core.Workspace.Current.Settings.Set("party_transport", "direct");
                Console.WriteLine("PASS: seamless settings render for host, join, single player and Steam without showing a window");

                // The other settings pages build and lay out too (their first and last screen).
                foreach (var type in new[] { typeof(DisplayPage), typeof(GraphicsPage), typeof(ControlsPage), typeof(TrophiesPage),
                             typeof(SettingsPage), typeof(ModsPage), typeof(CheatsPage), typeof(EnginePage) })
                {
                    var page = (LauncherPage)Activator.CreateInstance(type)!;
                    page.RaiseEvent(new RoutedEventArgs(FrameworkElement.LoadedEvent));
                    var content = (UIElement)page.Content;
                    var scroll = FindScroll(content) ?? throw new InvalidOperationException($"{type.Name} has no scroller.");
                    page.Content = null;
                    var surface = new Border { Background = new SolidColorBrush(Color.FromRgb(24, 24, 28)), Child = content };
                    surface.SetResourceReference(System.Windows.Documents.TextElement.ForegroundProperty, "TextFillColorPrimaryBrush");
                    surface.Measure(new Size(920, 760));
                    surface.Arrange(new Rect(0, 0, 920, 760));
                    surface.UpdateLayout();
                    foreach (var (label, fraction) in new[] { ("top", 0.0), ("end", 1.0) })
                    {
                        scroll.ScrollToVerticalOffset(scroll.ScrollableHeight * fraction);
                        surface.UpdateLayout();
                        var image = new RenderTargetBitmap(920, 760, 96, 96, PixelFormats.Pbgra32);
                        image.Render(surface);
                        var encoder = new PngBitmapEncoder();
                        encoder.Frames.Add(BitmapFrame.Create(image));
                        using var output = File.Create(Path.Combine(directory, $"{type.Name}-{label}.png"));
                        encoder.Save(output);
                    }
                }
                Console.WriteLine("PASS: every settings page renders");
            }
            catch (Exception error) { failure = error; }
        });
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        thread.Join();
        if (failure != null) throw new InvalidOperationException("Launcher rendering failed", failure);
    }

    private static IEnumerable<DependencyObject> Descendants(DependencyObject root)
    {
        for (var i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            yield return child;
            foreach (var descendant in Descendants(child)) yield return descendant;
        }
    }
    private static ScrollViewer? FindScroll(object at) => at as ScrollViewer ??
        (at is DependencyObject parent ? LogicalTreeHelper.GetChildren(parent).Cast<object>().Select(FindScroll).FirstOrDefault(s => s != null) : null);
}
