using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace WireSteam.UI;

public sealed class ConfigDialog : Window {
	private readonly TextBox name, editor;

	public string ProfileName => name.Text.Trim();
	public string Configuration => editor.Text;

	public ConfigDialog(Window owner, string title, string profileName, string content) {
		Owner = owner;
		Title = title.ToUpperInvariant();
		Width = 790;
		Height = 670;
		MinWidth = 600;
		MinHeight = 480;
		WindowStartupLocation = WindowStartupLocation.CenterOwner;
		Background = new SolidColorBrush(Color.FromRgb(9, 13, 21));
		Foreground = Brushes.White;
		FontFamily = TryFindResource("BodyFont") as FontFamily ?? new FontFamily("Consolas");

		var grid = new Grid { Margin = new Thickness(24) };
		grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
		grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
		grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
		grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
		var heading = new TextBlock { Text = Title, FontFamily = TryFindResource("HeadingFont") as FontFamily ?? FontFamily, FontSize = 21, FontWeight = FontWeights.Bold, Margin = new Thickness(0, 0, 0, 16) };
		grid.Children.Add(heading);

		name = new TextBox { Text = profileName, Margin = new Thickness(0, 0, 0, 14), Padding = new Thickness(12), MaxLength = 60 };
		name.Style = TryFindResource("Input") as Style;
		Grid.SetRow(name, 1);
		grid.Children.Add(name);

		editor = new TextBox {
			Text = content,
			AcceptsReturn = true,
			AcceptsTab = true,
			VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
			HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
			FontFamily = new FontFamily("Cascadia Code,Consolas"),
			FontSize = 13,
			Padding = new Thickness(14),
			TextWrapping = TextWrapping.NoWrap
		};
		editor.Style = TryFindResource("Input") as Style;
		editor.VerticalContentAlignment = VerticalAlignment.Top;
		Grid.SetRow(editor, 2);
		grid.Children.Add(editor);

		var footer = new DockPanel { Margin = new Thickness(0, 16, 0, 0) };
		var save = new Button { Content = "VALIDATE & SAVE", Padding = new Thickness(18, 9, 18, 9), IsDefault = true, MinWidth = 145 };
		save.Style = TryFindResource("PrimaryButton") as Style ?? save.Style;
		save.Click += (_, _) => {
			if (string.IsNullOrWhiteSpace(ProfileName)) {
				name.Focus();
				return;
			}

			DialogResult = true;
		};
		DockPanel.SetDock(save, Dock.Right);
		footer.Children.Add(save);

		var cancel = new Button { Content = "CANCEL", Padding = new Thickness(16, 9, 16, 9), Margin = new Thickness(0, 0, 10, 0), IsCancel = true };
		cancel.Style = TryFindResource("SecondaryButton") as Style;
		DockPanel.SetDock(cancel, Dock.Right);
		footer.Children.Add(cancel);
		footer.Children.Add(new TextBlock { Text = "CONTAINS PRIVATE KEYS. KEEP THIS CONFIGURATION PRIVATE.", FontSize = 11, Foreground = new SolidColorBrush(Color.FromRgb(125, 145, 168)), VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap });

		Grid.SetRow(footer, 3);
		grid.Children.Add(footer);
		Content = grid;
	}
}
