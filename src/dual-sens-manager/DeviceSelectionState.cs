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
        public DetectionTarget CurrentTarget { get; private set; }

        public string LeftDeviceId { get; private set; }

        public string RightDeviceId { get; private set; }

        public string LeftDeviceName { get; private set; }

        public string RightDeviceName { get; private set; }

        public DeviceSelectionState()
        {
            CurrentTarget = DetectionTarget.None;
            LeftDeviceId = string.Empty;
            RightDeviceId = string.Empty;
            LeftDeviceName = string.Empty;
            RightDeviceName = string.Empty;
        }

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


        public void ClearTarget(DetectionTarget target)
        {
            if (target == DetectionTarget.Left)
            {
                LeftDeviceId = string.Empty;
                LeftDeviceName = string.Empty;
            }
            else if (target == DetectionTarget.Right)
            {
                RightDeviceId = string.Empty;
                RightDeviceName = string.Empty;
            }
        }

        public bool HasBothDevices
        {
            get
            {
                return !string.IsNullOrWhiteSpace(LeftDeviceId) && !string.IsNullOrWhiteSpace(RightDeviceId);
            }
        }
    }
}
