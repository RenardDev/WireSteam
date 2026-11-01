using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace WireSteam.UI;

public sealed class PortForwardDialog : Window {
	private readonly PortForwardConfiguration configuration;
	private readonly ObservableCollection<PortForwardRule> rules;
	private readonly ListBox list;
	private readonly TextBlock error;
	private readonly Button add, edit, delete;
	public string Configuration {
		get;
		private set;
	} = "";

	public PortForwardDialog(Window owner, string profileName, string content) {
		configuration = new PortForwardConfiguration(content);
		rules = new ObservableCollection<PortForwardRule>(configuration.Rules);

		Owner = owner;
		Title = "PORT FORWARDING";
		Width = 800;
		Height = 560;
		MinWidth = 650;
		MinHeight = 440;
		WindowStartupLocation = WindowStartupLocation.CenterOwner;
		Background = Brush(9, 13, 21);
		Foreground = Brushes.White;
		FontFamily = TryFindResource("BodyFont") as FontFamily ?? new FontFamily("Consolas");

		var grid = new Grid { Margin = new Thickness(24) };
		foreach (var height in new[] { GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto, GridLength.Auto }) {
			grid.RowDefinitions.Add(new RowDefinition { Height = height });
		}

		var heading = new StackPanel();
		heading.Children.Add(new TextBlock { Text = "PORT FORWARDING", Style = TryFindResource("PageTitle") as Style });
		heading.Children.Add(new TextBlock { Text = profileName, Foreground = Brush(111, 255, 199), FontSize = 14, Margin = new Thickness(0, 7, 0, 0) });
		grid.Children.Add(heading);
		Add(grid, new TextBlock { Text = "MAKE LOCAL TCP OR UDP SERVICES REACHABLE AT THIS COMPUTER’S VPN ADDRESS. PEERS USE EACH RULE’S PROTOCOL AND LISTEN ENDPOINT WHILE THIS TUNNEL IS ACTIVE.", Foreground = Brush(145, 166, 189), TextWrapping = TextWrapping.Wrap, LineHeight = 19, Margin = new Thickness(0, 18, 0, 16) }, 1);
		list = new ListBox { ItemsSource = rules, DisplayMemberPath = nameof(PortForwardRule.DisplayText), Background = Brush(16, 24, 36), Foreground = Brushes.White, BorderBrush = Brush(29, 42, 58), Padding = new Thickness(8), FontSize = 13, HorizontalContentAlignment = HorizontalAlignment.Stretch };
		list.SetResourceReference(ItemsControl.ItemContainerStyleProperty, "ProfileItem");
		list.MouseDoubleClick += (_, _) => {
			if (list.SelectedItem is PortForwardRule) {
				EditSelected();
			}
		};

		Add(grid, list, 2);

		var actions = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 12, 0, 0) };
		add = Button("ADD FORWARD", () => EditRule(null));
		edit = Button("EDIT", EditSelected);
		delete = Button("DELETE", DeleteSelected);
		foreach (var button in new[] { add, edit, delete }) {
			button.Margin = new Thickness(0, 0, 9, 0);
			actions.Children.Add(button);
		}

		Add(grid, actions, 3);
		error = new TextBlock { Foreground = Brush(255, 118, 143), TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 12, 0, 0) };
		Add(grid, error, 4);

		var footer = new Grid { Margin = new Thickness(0, 18, 0, 0) };
		footer.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
		footer.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
		footer.Children.Add(new TextBlock { Text = "TCP / UDP · UP TO 32 RULES\nTARGETS STAY LOCAL: 127.X.X.X OR ::1", Foreground = Brush(125, 145, 168), FontSize = 10, LineHeight = 17, VerticalAlignment = VerticalAlignment.Center });
		var saveActions = new StackPanel { Orientation = Orientation.Horizontal };
		var cancel = Button("CANCEL", () => DialogResult = false);
		cancel.IsCancel = true;
		cancel.Margin = new Thickness(0, 0, 10, 0);
		var save = Button("VALIDATE & SAVE", Save);
		save.IsDefault = true;
		save.Style = TryFindResource("PrimaryButton") as Style;
		saveActions.Children.Add(cancel);
		saveActions.Children.Add(save);
		Grid.SetColumn(saveActions, 1);
		footer.Children.Add(saveActions);
		Add(grid, footer, 5);
		Content = grid;
		list.SelectionChanged += (_, _) => RefreshActions();
		RefreshActions();
	}

	private static SolidColorBrush Brush(byte red, byte green, byte blue) => new(Color.FromRgb(red, green, blue));

	private static void Add(Grid parent, UIElement child, int row) {
		Grid.SetRow(child, row);
		parent.Children.Add(child);
	}

	private Button Button(string text, Action click) {
		var button = new Button { Content = text, Style = TryFindResource("SecondaryButton") as Style, Padding = new Thickness(14, 9, 14, 9) };
		button.Click += (_, _) => click();

		return button;
	}

	private void RefreshActions() {
		add.IsEnabled = configuration.AllowsForwards && rules.Count < PortForwardConfiguration.MaximumRules && configuration.VpnAddresses.Count > 0;
		edit.IsEnabled = configuration.AllowsForwards && list.SelectedItem is PortForwardRule;
		delete.IsEnabled = list.SelectedItem is PortForwardRule;
		error.Text = !configuration.AllowsForwards ? "INTERNET-CLIENT PROFILES CANNOT HOST FORWARDS. USE A PROFILE WITH INTERNETMODE SET TO OFF OR SERVER ON THE COMPUTER RUNNING THE LOCAL SERVICE." : configuration.VpnAddresses.Count == 0 ? "THIS PROFILE HAS NO VPN IP ADDRESS. ADD AN INTERFACE ADDRESS IN EDIT PROFILE FIRST." : "";
	}

	private void EditSelected() {
		if (configuration.AllowsForwards && list.SelectedItem is PortForwardRule rule) {
			EditRule(rule);
		}
	}

	private void EditRule(PortForwardRule? original) {
		var dialog = new ForwardRuleDialog(this, configuration, rules, original);
		if (dialog.ShowDialog() != true || dialog.Rule == null) {
			return;
		}

		if (original == null) {
			rules.Add(dialog.Rule);
		} else {
			rules[rules.IndexOf(original)] = dialog.Rule;
		}

		list.SelectedItem = dialog.Rule;
		RefreshActions();
	}

	private void DeleteSelected() {
		if (list.SelectedItem is PortForwardRule rule) {
			int index = rules.IndexOf(rule);
			rules.Remove(rule);

			if (rules.Count > 0) {
				list.SelectedIndex = Math.Min(index, rules.Count - 1);
			}

			RefreshActions();
		}
	}

	private void Save() {
		try {
			Configuration = configuration.Apply(rules.ToArray());
			DialogResult = true;
		} catch (ArgumentException exception) {
			error.Text = exception.Message.ToUpperInvariant();
		}
	}

	internal sealed class ForwardRuleDialog : Window {
		private readonly ComboBox address, protocol;
		private readonly TextBox listenPort, targetAddress, targetPort;
		private readonly TextBlock error;
		private readonly PortForwardConfiguration configuration;
		private readonly IReadOnlyCollection<PortForwardRule> existing;
		private readonly PortForwardRule? original;
		internal PortForwardRule? Rule {
			get;
			private set;
		}

		internal ForwardRuleDialog(Window owner, PortForwardConfiguration configuration, IReadOnlyCollection<PortForwardRule> existing, PortForwardRule? original) {
			this.configuration = configuration;
			this.existing = existing;
			this.original = original;
			Owner = owner;
			Title = original == null ? "ADD PORT FORWARD" : "EDIT PORT FORWARD";
			Width = 650;
			Height = 490;
			MinWidth = 590;
			MinHeight = 450;
			WindowStartupLocation = WindowStartupLocation.CenterOwner;
			Background = Brush(9, 13, 21);
			Foreground = Brushes.White;
			FontFamily = TryFindResource("BodyFont") as FontFamily ?? new FontFamily("Consolas");

			var grid = new Grid { Margin = new Thickness(24) };
			foreach (var height in new[] { GridLength.Auto, GridLength.Auto, GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto }) {
				grid.RowDefinitions.Add(new RowDefinition { Height = height });
			}

			grid.Children.Add(new TextBlock { Text = Title, Style = TryFindResource("PageTitle") as Style });
			var transport = new Grid { Margin = new Thickness(0, 14, 0, 0) };
			transport.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
			transport.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(140) });
			transport.Children.Add(new TextBlock { Text = "VPN LISTENER → LOCAL SERVICE", Foreground = Brush(111, 255, 199), VerticalAlignment = VerticalAlignment.Center });
			protocol = new ComboBox { ItemsSource = new[] { "TCP", "UDP" }, SelectedItem = original?.Protocol.ToUpperInvariant() ?? "TCP", Style = TryFindResource("SelectInput") as Style };
			protocol.ItemTemplate = new DataTemplate { VisualTree = new FrameworkElementFactory(typeof(TextBlock)) };
			protocol.ItemTemplate.VisualTree.SetBinding(TextBlock.TextProperty, new System.Windows.Data.Binding());
			var protocolField = Field("PROTOCOL", protocol);
			protocolField.Margin = new Thickness(14, 0, 0, 0);
			Grid.SetColumn(protocolField, 1);
			transport.Children.Add(protocolField);
			Add(grid, transport, 1);

			var listen = new Grid { Margin = new Thickness(0, 18, 0, 0) };
			listen.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
			listen.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(140) });
			address = new ComboBox { ItemsSource = configuration.VpnAddresses, SelectedIndex = 0, Style = TryFindResource("SelectInput") as Style, MinWidth = 240 };
			address.ItemTemplate = new DataTemplate { VisualTree = new FrameworkElementFactory(typeof(TextBlock)) };
			address.ItemTemplate.VisualTree.SetBinding(TextBlock.TextProperty, new System.Windows.Data.Binding());
			listenPort = Input("25565");
			listen.Children.Add(Field("VPN ADDRESS", address));
			var listenPortField = Field("LISTEN PORT", listenPort);
			listenPortField.Margin = new Thickness(14, 0, 0, 0);
			Grid.SetColumn(listenPortField, 1);
			listen.Children.Add(listenPortField);
			Add(grid, listen, 2);

			var target = new Grid { Margin = new Thickness(0, 18, 0, 0) };
			target.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
			target.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(140) });
			targetAddress = Input("127.0.0.1");
			targetPort = Input("25565");
			target.Children.Add(Field("LOCAL LOOPBACK ADDRESS", targetAddress));
			var targetPortField = Field("TARGET PORT", targetPort);
			targetPortField.Margin = new Thickness(14, 0, 0, 0);
			Grid.SetColumn(targetPortField, 1);
			target.Children.Add(targetPortField);
			Add(grid, target, 3);

			if (original != null) {
				var from = PortForwardConfiguration.ParseEndpoint(original.Listen);
				var to = PortForwardConfiguration.ParseEndpoint(original.Target);
				address.SelectedItem = from.Address.ToString();
				listenPort.Text = from.Port.ToString(CultureInfo.InvariantCulture);
				targetAddress.Text = to.Address.ToString();
				targetPort.Text = to.Port.ToString(CultureInfo.InvariantCulture);
			}

			error = new TextBlock { Foreground = Brush(255, 118, 143), TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 15, 0, 10), FontSize = 11 };
			Add(grid, error, 4);
			var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
			var cancel = new Button { Content = "CANCEL", IsCancel = true, Style = TryFindResource("SecondaryButton") as Style, Margin = new Thickness(0, 0, 10, 0) };
			var save = new Button { Content = original == null ? "ADD FORWARD" : "APPLY CHANGES", IsDefault = true, Style = TryFindResource("PrimaryButton") as Style };
			save.Click += (_, _) => Save();
			buttons.Children.Add(cancel);
			buttons.Children.Add(save);
			Add(grid, buttons, 5);
			Content = grid;
		}

		private TextBox Input(string value) => new() { Text = value, Style = TryFindResource("Input") as Style };

		private static StackPanel Field(string label, UIElement control) {
			var panel = new StackPanel();
			panel.Children.Add(new TextBlock { Text = label, FontSize = 10, Foreground = Brush(145, 166, 189), Margin = new Thickness(0, 0, 0, 7) });
			panel.Children.Add(control);

			return panel;
		}

		private void Save() {
			try {
				if (address.SelectedItem is not string ip || !int.TryParse(listenPort.Text, NumberStyles.None, CultureInfo.InvariantCulture, out int listenPortValue) || !int.TryParse(targetPort.Text, NumberStyles.None, CultureInfo.InvariantCulture, out int targetPortValue)) {
					throw new ArgumentException("Select a VPN address and enter numeric ports between 1 and 65535.");
				}

				var candidate = new PortForwardRule(PortForwardConfiguration.Endpoint(ip, listenPortValue), PortForwardConfiguration.Endpoint(targetAddress.Text.Trim(), targetPortValue), protocol.SelectedItem as string ?? "TCP") { SourceIndex = original?.SourceIndex ?? -1 };
				configuration.Validate(existing.Where(rule => !ReferenceEquals(rule, original)).Append(candidate).ToArray());
				Rule = candidate;
				DialogResult = true;
			} catch (ArgumentException exception) {
				error.Text = exception.Message.ToUpperInvariant();
			}
		}
	}
}
