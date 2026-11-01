using System;
using System.Collections.Generic;
using System.Threading.Tasks;

namespace WireSteam.UI.Services;

// Offline preview/test implementation. It cannot start a process or access a private profile.
internal sealed class PreviewBackend : IBackend {
	public string EnginePath => "OFFLINE PREVIEW";
	public string ProfilesDirectory => "OFFLINE PREVIEW";

	public event EventHandler? StatusChanged {
		add {
		}

		remove {
		}
	}

	private static Exception Offline() => new InvalidOperationException("This is an offline design preview.");

	public Task<IReadOnlyList<Profile>> LoadProfilesAsync() => Task.FromResult<IReadOnlyList<Profile>>(Array.Empty<Profile>());
	public Task<bool> CheckPrerequisitesAsync() => Task.FromResult(false);
	public Task<Profile> ImportAsync(string sourcePath) => throw Offline();
	public Task ReloadAsync(Profile profile) => throw Offline();
	public Task SaveAsync(Profile profile, string content) => throw Offline();
	public Task<Profile> CreateAsync(string name, string content) => throw Offline();
	public Task<string> CreateTemplateAsync() => throw Offline();
	public Task<string> ReadConfigAsync(Profile profile) => throw Offline();
	public Task ExportAsync(Profile profile, string destination, bool overwrite = false) => throw Offline();
	public Task RemoveAsync(Profile profile) => throw Offline();

	public Task StartAsync(Profile profile) => throw Offline();
	public Task StopAsync(Profile profile) => throw Offline();
	public Task StopAllAsync() => Task.CompletedTask;

	public void Dispose() {
	}
}
