using System;
using System.Collections.Generic;
using System.Threading.Tasks;

namespace WireSteam.UI.Services;

public interface IBackend : IDisposable {
	string EnginePath {
		get;
	}

	string ProfilesDirectory {
		get;
	}

	event EventHandler? StatusChanged;

	Task<IReadOnlyList<Profile>> LoadProfilesAsync();
	Task<Profile> ImportAsync(string sourcePath);
	Task ReloadAsync(Profile profile);
	Task SaveAsync(Profile profile, string content);
	Task<Profile> CreateAsync(string name, string content);
	Task<string> CreateTemplateAsync();
	Task<string> ReadConfigAsync(Profile profile);
	Task ExportAsync(Profile profile, string destination, bool overwrite = false);
	Task RemoveAsync(Profile profile);

	Task StartAsync(Profile profile);
	Task StopAsync(Profile profile);
	Task StopAllAsync();

	Task<bool> CheckPrerequisitesAsync();
}
