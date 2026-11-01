using System;
using System.Globalization;
using System.Windows.Data;

namespace WireSteam.UI.Controls;

/// <summary>Presentation casing for UI action/status labels; never applied to configuration values.</summary>
public sealed class UppercaseConverter : IValueConverter {
	public object Convert(object value, Type targetType, object parameter, CultureInfo culture) => value is string text ? text.ToUpperInvariant() : value is bool boolean ? (boolean ? "TRUE" : "FALSE") : value;

	public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) => Binding.DoNothing;
}
