using System;

namespace DualSensManager
{
    internal enum DetectionTarget
    {
        None,
        Left,
        Right
    }

    internal sealed class DeviceSelectionState
    {
        public DetectionTarget CurrentTarget { get; private set; } = DetectionTarget.None;

        public string LeftDeviceId { get; private set; } = string.Empty;

        public string RightDeviceId { get; private set; } = string.Empty;

        public string LeftDeviceName { get; private set; } = string.Empty;

        public string RightDeviceName { get; private set; } = string.Empty;

        public void BeginDetection(DetectionTarget target)
        {
            CurrentTarget = target;
        }

        public bool ApplyDetectionResult(string deviceId, string deviceName)
        {
            if (CurrentTarget == DetectionTarget.None)
            {
                return false;
            }

            if (CurrentTarget == DetectionTarget.Left)
            {
                LeftDeviceId = deviceId ?? string.Empty;
                LeftDeviceName = deviceName ?? string.Empty;
            }
            else if (CurrentTarget == DetectionTarget.Right)
            {
                RightDeviceId = deviceId ?? string.Empty;
                RightDeviceName = deviceName ?? string.Empty;
            }

            CurrentTarget = DetectionTarget.None;
            return true;
        }

        public bool HasBothDevices =>
            !string.IsNullOrWhiteSpace(LeftDeviceId) &&
            !string.IsNullOrWhiteSpace(RightDeviceId);
    }
}