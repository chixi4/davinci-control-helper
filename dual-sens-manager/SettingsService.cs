using System;
using System.Collections.Generic;
using System.Linq;

namespace DualSensManager
{
    internal sealed class SettingsService
    {
        private const double NormalizedDpi = 1000.0;

        public DriverConfig LoadActiveConfig()
        {
            VersionHelper.ValidOrThrow();
            return DriverConfig.GetActive();
        }

        public List<MultiHandleDevice> EnumerateDevices()
        {
            var result = new List<MultiHandleDevice>();
            foreach (var device in MultiHandleDevice.GetList())
            {
                result.Add(device);
            }

            return result;
        }

        public Dictionary<IntPtr, MultiHandleDevice> BuildHandleLookup(IEnumerable<MultiHandleDevice> devices)
        {
            var map = new Dictionary<IntPtr, MultiHandleDevice>();
            foreach (var device in devices)
            {
                foreach (var handle in device.handles)
                {
                    if (!map.ContainsKey(handle))
                    {
                        map[handle] = device;
                    }
                }
            }

            return map;
        }

        public double GetMultiplier(Profile profile)
        {
            return profile.outputDPI / NormalizedDpi;
        }

        public ApplyResult Apply(string rightId, string rightName, string leftId, string leftName, double leftMultiplier)
        {
            if (string.IsNullOrWhiteSpace(rightId))
            {
                throw new ArgumentException("必须先指定右手鼠标", nameof(rightId));
            }

            if (string.IsNullOrWhiteSpace(leftId))
            {
                throw new ArgumentException("必须先指定左手鼠标", nameof(leftId));
            }

            if (leftMultiplier <= 0)
            {
                throw new ArgumentOutOfRangeException(nameof(leftMultiplier), "sens multiplier 必须大于 0");
            }

            var config = LoadActiveConfig();
            if (config.profiles.Count == 0)
            {
                throw new InvalidOperationException("驱动中未找到可用的 Profile");
            }

            var baseProfile = config.profiles[0];
            var baseMultiplier = GetMultiplier(baseProfile);

            var leftProfileName = BuildLeftProfileName(baseProfile.name);
            var leftProfile = CloneProfile(config.accels[0], leftProfileName, leftMultiplier);

            var leftIndex = FindProfileIndex(config, leftProfileName);
            if (leftIndex >= 0)
            {
                config.SetProfileAt(leftIndex, leftProfile);
            }
            else
            {
                config.profiles.Add(leftProfile);
                config.accels.Add(new ManagedAccel(leftProfile));
            }

            UpdateDevice(config, rightId, rightName, baseProfile.name);
            UpdateDevice(config, leftId, leftName, leftProfileName);

            var errors = config.Errors();
            if (!string.IsNullOrEmpty(errors))
            {
                throw new InvalidOperationException(errors);
            }

            config.Activate();

            return new ApplyResult(baseMultiplier, leftMultiplier);
        }

        private static string BuildLeftProfileName(string baseName)
        {
            if (string.IsNullOrWhiteSpace(baseName))
            {
                return "LeftHand";
            }

            var trimmed = baseName.Trim();
            if (trimmed.EndsWith("(Left)", StringComparison.OrdinalIgnoreCase))
            {
                return trimmed;
            }

            return $"{trimmed} (Left)";
        }

        private static int FindProfileIndex(DriverConfig config, string name)
        {
            for (int i = 0; i < config.profiles.Count; i++)
            {
                if (string.Equals(config.profiles[i].name, name, StringComparison.OrdinalIgnoreCase))
                {
                    return i;
                }
            }

            return -1;
        }

        private static Profile CloneProfile(ManagedAccel sourceAccel, string profileName, double multiplier)
        {
            var profile = sourceAccel.Settings;
            profile.name = profileName;
            profile.outputDPI = multiplier * NormalizedDpi;
            return profile;
        }

        private static void UpdateDevice(DriverConfig config, string deviceId, string deviceName, string profileName)
        {
            var target = config.devices.FirstOrDefault(d => d.id == deviceId);
            if (target == null)
            {
                target = new DeviceSettings
                {
                    id = deviceId,
                    name = string.IsNullOrWhiteSpace(deviceName) ? deviceId : deviceName,
                    config = config.defaultDeviceConfig,
                };
                config.devices.Add(target);
            }

            if (string.IsNullOrWhiteSpace(target.name) && !string.IsNullOrWhiteSpace(deviceName))
            {
                target.name = deviceName;
            }

            target.profile = profileName;
        }
    }

    internal readonly struct ApplyResult
    {
        public ApplyResult(double rightMultiplier, double leftMultiplier)
        {
            RightMultiplier = rightMultiplier;
            LeftMultiplier = leftMultiplier;
        }

        public double RightMultiplier { get; }

        public double LeftMultiplier { get; }
    }
}