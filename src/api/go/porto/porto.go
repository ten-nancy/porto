package porto

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"io"
	"math"
	"net"
	"os"
	"syscall"
	"time"

	"github.com/ten-nancy/porto/src/api/go/porto/pkg/rpc"

	"google.golang.org/protobuf/proto"
)

const (
	PortoSocket        = "/run/portod.socket"
	DefaultTimeout     = 5 * time.Minute
	DefaultStopTimeout = 5 * time.Minute
)

type TProperty struct {
	Name        string
	Description string
}

type TData struct {
	Name        string
	Description string
}

type TVolumeDescription struct {
	Path       *string
	Properties map[string]string
	Containers []string
	Links      []*rpc.TVolumeLink
}

type TStorageDescription struct {
	Name         string
	OwnerUser    string
	OwnerGroup   string
	LastUsage    uint64
	PrivateValue string
}

type TLayerDescription struct {
	Name         string
	OwnerUser    string
	OwnerGroup   string
	LastUsage    uint64
	PrivateValue string
}

type TPortoGetResponse struct {
	Value    string
	Error    int
	ErrorMsg string
}

type DockerImage struct {
	Name     string
	Place    string
	Platform string
}

type DockerRegistryCredentials struct {
	AuthToken   string
	AuthPath    string
	AuthService string
}

type PortoError struct {
	Code    rpc.EError
	Message string
}

func (err *PortoError) Error() string {
	return fmt.Sprintf("%s: %s", rpc.EError_name[int32(err.Code)], err.Message)
}

type PortoAPI interface {
	// Connection
	Connect() error
	Close() error
	IsConnected() bool
	SetAutoReconnect(bool)

	// Timeout
	GetTimeout() time.Duration
	SetTimeout(timeout time.Duration)

	// Transport
	WriteRequest(*rpc.TContainerRequest) error
	ReadResponse() (*rpc.TContainerResponse, error)
	CallTimeout(*rpc.TContainerRequest, time.Duration) (*rpc.TContainerResponse, error)
	Call(*rpc.TContainerRequest) (*rpc.TContainerResponse, error)

	GetVersion() (string, string, error)

	// ContainerAPI
	Create(name string) error
	CreateWeak(name string) error
	Destroy(name string) error

	Start(name string) error
	Stop(name string) error
	StopTimeout(name string, timeout time.Duration) error
	Kill(name string, sig syscall.Signal) error
	Pause(name string) error
	Resume(name string) error

	Wait(containers []string, timeout time.Duration) (string, error)

	List() ([]string, error)
	ListContainers(mask string) ([]string, error)
	Plist() ([]TProperty, error)
	Dlist() ([]TData, error)

	Get(names []string, properties []string) (*rpc.TContainerGetResponse, error)
	GetProperties(names []string, properties []string) (map[string]map[string]string, error)

	GetProperty(name string, property string) (string, error)
	SetProperty(name string, property string, value string) error

	GetData(name string, data string) (string, error)

	// SpecAPI
	CreateFromSpec(spec *rpc.TContainerSpec, volumes []*rpc.TVolumeSpec, start bool) error
	UpdateFromSpec(spec *rpc.TContainerSpec, start bool) error

	// VolumeAPI
	ListVolumes(path string, container string) ([]*TVolumeDescription, error)
	GetVolume(path string) (*TVolumeDescription, error)
	ListVolumeProperties() ([]TProperty, error)
	CreateVolume(path string, config map[string]string) (*TVolumeDescription, error)
	TuneVolume(path string, config map[string]string) error

	LinkVolume(path string, container string) error
	UnlinkVolume(path string, container string) error
	LinkVolumeTarget(path string, container string, target string, required bool, readOnly bool) error
	UnlinkVolumeTarget(path string, container string, target string) error
	UnlinkVolumeStrict(path string, container string, target string, strict bool) error

	// LayerAPI
	ImportLayer(layer string, tarball string, merge bool) error
	ImportLayer4(layer string, tarball string, merge bool,
		place string, privateValue string) error
	ExportLayer(volume string, tarball string) error
	RemoveLayer(layer string) error
	RemoveLayer2(layer string, place string) error
	ListLayers() ([]string, error)
	ListLayers2(place string, mask string) ([]TLayerDescription, error)

	GetLayerPrivate(layer string, place string) (string, error)
	SetLayerPrivate(layer string, place string, privateValue string) error

	ListStorage(place string, mask string) ([]TStorageDescription, error)
	RemoveStorage(name string, place string) error

	ConvertPath(path string, src string, dest string) (string, error)
	AttachProcess(name string, pid uint32, comm string) error

	DockerImageStatus(name, place string) (*rpc.TDockerImage, error)
	ListDockerImages(place, mask string) ([]*rpc.TDockerImage, error)
	PullDockerImage(image DockerImage, creds DockerRegistryCredentials) (*rpc.TDockerImage, error)
	RemoveDockerImage(name, place string) error
}

type client struct {
	conn        net.Conn
	reader      *bufio.Reader
	noReconnect bool
	timeout     time.Duration
}

func Dial() (PortoAPI, error) {
	c := new(client)
	c.SetTimeout(DefaultTimeout)
	return c, nil
}

// Connect connects to the address of portod unix domain socket
//
// For testing purposes inside containers default path to Portod
// socket could be replaced by environment variable PORTO_SOCKET.
func (c *client) Connect() error {
	_ = c.Close()

	portoSocketPath := PortoSocket
	rv, ok := os.LookupEnv("PORTO_SOCKET")
	if ok {
		portoSocketPath = rv
	}
	conn, err := net.DialTimeout("unix", portoSocketPath, c.GetTimeout())
	if err == nil {
		c.conn = conn
		c.reader = bufio.NewReader(c.conn)
	}
	return err
}

func (c *client) Close() error {
	if c.conn == nil {
		return nil
	}
	err := c.conn.Close()
	c.conn = nil
	c.reader = nil
	return err
}

func (c *client) IsConnected() bool {
	return c.conn != nil
}

func (c *client) SetAutoReconnect(reconnect bool) {
	c.noReconnect = !reconnect
}

func (c *client) GetTimeout() time.Duration {
	return c.timeout
}

func (c *client) SetTimeout(timeout time.Duration) {
	c.timeout = timeout
}

func optString(val string) *string {
	if val == "" {
		return nil
	}
	return &val
}

func (c *client) WriteRequest(req *rpc.TContainerRequest) error {
	buf, err := proto.Marshal(req)
	if err != nil {
		return err
	}

	len := len(buf)
	hdr := make([]byte, 64)
	hdrLen := binary.PutUvarint(hdr, uint64(len))

	_, err = c.conn.Write(hdr[:hdrLen])
	if err != nil {
		_ = c.Close()
		return err
	}

	_, err = c.conn.Write(buf)
	if err != nil {
		_ = c.Close()
		return err
	}

	return nil
}

func (c *client) ReadResponse() (*rpc.TContainerResponse, error) {
	len, err := binary.ReadUvarint(c.reader)
	if err != nil {
		_ = c.Close()
		return nil, err
	}

	buf := make([]byte, len)
	_, err = io.ReadFull(c.reader, buf)
	if err != nil {
		_ = c.Close()
		return nil, err
	}

	rsp := new(rpc.TContainerResponse)
	err = proto.Unmarshal(buf, rsp)
	if err != nil {
		_ = c.Close()
		return nil, err
	}

	if rsp.GetError() != rpc.EError_Success {
		return rsp, &PortoError{
			Code:    rsp.GetError(),
			Message: rsp.GetErrorMsg(),
		}
	}

	return rsp, nil
}

func (c *client) CallTimeout(req *rpc.TContainerRequest, timeout time.Duration) (*rpc.TContainerResponse, error) {
	if !c.IsConnected() {
		if c.noReconnect {
			err := &PortoError{
				Code:    rpc.EError_SocketError,
				Message: "Socket not connected",
			}
			return nil, err
		}
		err := c.Connect()
		if err != nil {
			return nil, err
		}
	}

	reqTimeout := c.GetTimeout()
	if reqTimeout < 0 {
		err := c.conn.SetDeadline(time.Time{})
		if err != nil {
			return nil, err
		}
	} else {
		deadline := time.Now().Add(reqTimeout)
		err := c.conn.SetWriteDeadline(deadline)
		if err != nil {
			return nil, err
		}

		if timeout < 0 {
			deadline = time.Time{}
		} else {
			deadline = deadline.Add(timeout)
		}

		err = c.conn.SetReadDeadline(deadline)
		if err != nil {
			return nil, err
		}
	}

	err := c.WriteRequest(req)
	if err != nil {
		return nil, err
	}

	rsp, err := c.ReadResponse()
	if err != nil {
		return nil, err
	}

	return rsp, nil
}

func (c *client) Call(req *rpc.TContainerRequest) (*rpc.TContainerResponse, error) {
	return c.CallTimeout(req, 0)
}

func (c *client) GetVersion() (string, string, error) {
	req := &rpc.TContainerRequest{
		Version: new(rpc.TVersionRequest),
	}
	resp, err := c.Call(req)
	if err != nil {
		return "", "", err
	}

	return resp.GetVersion().GetTag(), resp.GetVersion().GetRevision(), nil
}

// ContainerAPI
func (c *client) Create(name string) error {
	req := &rpc.TContainerRequest{
		Create: &rpc.TContainerCreateRequest{
			Name: &name,
		},
	}
	_, err := c.Call(req)
	return err
}

func (c *client) CreateWeak(name string) error {
	req := &rpc.TContainerRequest{
		CreateWeak: &rpc.TContainerCreateRequest{
			Name: &name,
		},
	}
	_, err := c.Call(req)
	return err
}

func (c *client) Destroy(name string) error {
	req := &rpc.TContainerRequest{
		Destroy: &rpc.TContainerDestroyRequest{
			Name: &name,
		},
	}
	_, err := c.Call(req)
	return err
}

func (c *client) Start(name string) error {
	req := &rpc.TContainerRequest{
		Start: &rpc.TContainerStartRequest{
			Name: &name,
		},
	}
	_, err := c.Call(req)
	if err != nil {
		return err
	}

	return nil
}

func (c *client) Stop(name string) error {
	return c.StopTimeout(name, DefaultStopTimeout)
}

func (c *client) StopTimeout(name string, timeout time.Duration) error {
	req := &rpc.TContainerRequest{
		Stop: &rpc.TContainerStopRequest{
			Name: &name,
		},
	}

	if timeout >= 0 {
		if timeout/time.Millisecond > math.MaxUint32 {
			return fmt.Errorf("timeout must be less than %d ms", math.MaxUint32)
		}

		timeoutms := uint32(timeout / time.Millisecond)
		req.Stop.TimeoutMs = &timeoutms
	}

	_, err := c.CallTimeout(req, timeout)
	if err != nil {
		return err
	}

	return nil
}

func (c *client) Kill(name string, sig syscall.Signal) error {
	signum := int32(sig)
	req := &rpc.TContainerRequest{
		Kill: &rpc.TContainerKillRequest{
			Name: &name,
			Sig:  &signum,
		},
	}
	_, err := c.Call(req)
	return err
}

func (c *client) Pause(name string) error {
	req := &rpc.TContainerRequest{
		Pause: &rpc.TContainerPauseRequest{
			Name: &name,
		},
	}
	_, err := c.Call(req)
	return err

}

func (c *client) Resume(name string) error {
	req := &rpc.TContainerRequest{
		Resume: &rpc.TContainerResumeRequest{
			Name: &name,
		},
	}
	_, err := c.Call(req)
	return err
}

func (c *client) Wait(containers []string, timeout time.Duration) (string, error) {
	req := &rpc.TContainerRequest{
		Wait: &rpc.TContainerWaitRequest{
			Name: containers,
		},
	}

	if timeout >= 0 {
		if timeout/time.Millisecond > math.MaxUint32 {
			return "", fmt.Errorf("timeout must be less than %d ms", math.MaxUint32)
		}

		timeoutms := uint32(timeout / time.Millisecond)
		req.Wait.TimeoutMs = &timeoutms
	}

	resp, err := c.Call(req)
	if err != nil {
		return "", err
	}

	return resp.GetWait().GetName(), nil
}

func (c *client) List() ([]string, error) {
	return c.ListContainers("")
}

func (c *client) ListContainers(mask string) ([]string, error) {
	req := &rpc.TContainerRequest{
		List: &rpc.TContainerListRequest{},
	}

	if mask != "" {
		req.List.Mask = &mask
	}

	resp, err := c.Call(req)
	if err != nil {
		return nil, err
	}

	return resp.GetList().GetName(), nil
}

func (c *client) Plist() (ret []TProperty, err error) {
	req := &rpc.TContainerRequest{
		PropertyList: new(rpc.TContainerPropertyListRequest),
	}
	resp, err := c.Call(req)
	for _, property := range resp.GetPropertyList().GetList() {
		var p = TProperty{
			Name:        property.GetName(),
			Description: property.GetDesc(),
		}
		ret = append(ret, p)
	}
	return ret, err
}

func (c *client) Dlist() (ret []TData, err error) {
	req := &rpc.TContainerRequest{
		DataList: new(rpc.TContainerDataListRequest),
	}
	resp, err := c.Call(req)
	if err != nil {
		return nil, err
	}

	for _, data := range resp.GetDataList().GetList() {
		var p = TData{
			Name:        data.GetName(),
			Description: data.GetDesc(),
		}
		ret = append(ret, p)
	}

	return ret, nil
}

func (c *client) Get(containers []string, properties []string) (ret *rpc.TContainerGetResponse, err error) {
	req := &rpc.TContainerRequest{
		Get: &rpc.TContainerGetRequest{
			Name:     containers,
			Variable: properties,
		},
	}

	rsp, err := c.Call(req)
	if err != nil {
		return nil, err
	}

	return rsp.GetGet(), nil
}

func (c *client) GetProperties(containers []string, properties []string) (ret map[string]map[string]string, _ error) {
	ret = make(map[string]map[string]string)
	res, err := c.Get(containers, properties)
	if err != nil {
		return nil, err
	}
	for _, item := range res.GetList() {
		ret[item.GetName()] = make(map[string]string)
		for _, value := range item.GetKeyval() {
			ret[item.GetName()][value.GetVariable()] = value.GetValue()
		}
	}
	return ret, nil
}

func (c *client) GetProperty(name string, property string) (string, error) {
	req := &rpc.TContainerRequest{
		GetProperty: &rpc.TContainerGetPropertyRequest{
			Name:     &name,
			Property: &property,
		},
	}

	resp, err := c.Call(req)
	if err != nil {
		return "", err
	}

	return resp.GetGetProperty().GetValue(), nil
}

func (c *client) SetProperty(name string, property string, value string) error {
	req := &rpc.TContainerRequest{
		SetProperty: &rpc.TContainerSetPropertyRequest{
			Name:     &name,
			Property: &property,
			Value:    &value,
		},
	}
	_, err := c.Call(req)
	return err
}

func (c *client) GetData(name string, data string) (string, error) {
	req := &rpc.TContainerRequest{
		GetData: &rpc.TContainerGetDataRequest{
			Name: &name,
			Data: &data,
		},
	}

	resp, err := c.Call(req)
	if err != nil {
		return "", err
	}

	return resp.GetGetData().GetValue(), nil
}

// SpecAPI
func (c *client) CreateFromSpec(spec *rpc.TContainerSpec, volumes []*rpc.TVolumeSpec, start bool) error {
	req := &rpc.TContainerRequest{
		CreateFromSpec: &rpc.TCreateFromSpecRequest{
			Container: spec,
			Volumes:   volumes,
			Start:     &start,
		},
	}
	_, err := c.Call(req)
	return err
}

func (c *client) UpdateFromSpec(spec *rpc.TContainerSpec, start bool) error {
	req := &rpc.TContainerRequest{
		UpdateFromSpec: &rpc.TUpdateFromSpecRequest{
			Container: spec,
			Start:     &start,
		},
	}
	_, err := c.Call(req)
	return err
}

// VolumeAPI
func (c *client) ListVolumes(path string, container string) (ret []*TVolumeDescription, err error) {
	req := &rpc.TContainerRequest{
		ListVolumes: &rpc.TVolumeListRequest{
			Path:      optString(path),
			Container: optString(container),
		},
	}

	resp, err := c.Call(req)
	if err != nil {
		return nil, err
	}

	for _, volume := range resp.GetVolumeList().GetVolumes() {
		desc := new(TVolumeDescription)
		desc.Path = volume.Path
		desc.Containers = append(desc.Containers, volume.GetContainers()...)
		desc.Properties = make(map[string]string)

		for _, property := range volume.GetProperties() {
			k := property.GetName()
			v := property.GetValue()
			desc.Properties[k] = v
		}
		ret = append(ret, desc)
	}
	return ret, err
}

func (c *client) GetVolume(path string) (*TVolumeDescription, error) {
	req := &rpc.TContainerRequest{
		ListVolumes: &rpc.TVolumeListRequest{
			Path: &path,
		},
	}
	rsp, err := c.Call(req)
	if err != nil {
		return nil, err
	}
	volume := rsp.GetVolumeList().GetVolumes()[0]
	ret := TVolumeDescription{
		Path:       volume.Path,
		Containers: volume.GetContainers(),
		Properties: make(map[string]string),
		Links:      volume.Links,
	}
	for _, property := range volume.GetProperties() {
		k := property.GetName()
		v := property.GetValue()
		ret.Properties[k] = v
	}

	return &ret, nil
}

func (c *client) ListVolumeProperties() (ret []TProperty, err error) {
	req := &rpc.TContainerRequest{
		ListVolumeProperties: &rpc.TVolumePropertyListRequest{},
	}

	resp, err := c.Call(req)
	if err != nil {
		return nil, err
	}

	for _, property := range resp.GetVolumePropertyList().GetProperties() {
		var desc = TProperty{
			Name:        property.GetName(),
			Description: property.GetDesc(),
		}
		ret = append(ret, desc)
	}
	return ret, err
}

func (c *client) CreateVolume(path string, config map[string]string) (desc *TVolumeDescription, err error) {
	var properties []*rpc.TVolumeProperty
	desc = new(TVolumeDescription)

	for k, v := range config {
		// NOTE: `k`, `v` save their addresses during `range`.
		// If we append pointers to them into an array,
		// all elements in the array will be the same.
		// So a pointer to the copy must be used.
		name, value := k, v
		prop := &rpc.TVolumeProperty{Name: &name, Value: &value}
		properties = append(properties, prop)
	}

	req := &rpc.TContainerRequest{
		CreateVolume: &rpc.TVolumeCreateRequest{
			Properties: properties,
		},
	}

	if path != "" {
		req.CreateVolume.Path = &path
	}

	resp, err := c.Call(req)
	if err != nil {
		return desc, err
	}

	volume := resp.GetVolumeDescription()
	desc.Path = volume.Path
	desc.Containers = append(desc.Containers, volume.GetContainers()...)
	desc.Properties = make(map[string]string, len(volume.GetProperties()))

	for _, property := range volume.GetProperties() {
		k := property.GetName()
		v := property.GetValue()
		desc.Properties[k] = v
	}

	return desc, err
}

func (c *client) TuneVolume(path string, config map[string]string) error {
	var properties []*rpc.TVolumeProperty
	for k, v := range config {
		name, value := k, v
		prop := &rpc.TVolumeProperty{Name: &name, Value: &value}
		properties = append(properties, prop)
	}
	req := &rpc.TContainerRequest{
		TuneVolume: &rpc.TVolumeTuneRequest{
			Path:       &path,
			Properties: properties,
		},
	}
	_, err := c.Call(req)
	return err
}

func (c *client) LinkVolume(path string, container string) error {
	req := &rpc.TContainerRequest{
		LinkVolume: &rpc.TVolumeLinkRequest{
			Path:      &path,
			Container: &container,
		},
	}
	_, err := c.Call(req)
	return err
}

func (c *client) LinkVolumeTarget(path string, container string, target string, required bool, readOnly bool) error {
	req := &rpc.TContainerRequest{
		LinkVolume: &rpc.TVolumeLinkRequest{
			Path:      &path,
			Container: &container,
			Target:    optString(target),
			Required:  &required,
			ReadOnly:  &readOnly,
		},
	}
	_, err := c.Call(req)
	return err
}

func (c *client) UnlinkVolume(path string, container string) error {
	req := &rpc.TContainerRequest{
		UnlinkVolume: &rpc.TVolumeUnlinkRequest{
			Path:      &path,
			Container: optString(container),
		},
	}
	_, err := c.Call(req)
	return err
}

func (c *client) UnlinkVolumeTarget(path string, container string, target string) error {
	req := &rpc.TContainerRequest{
		UnlinkVolumeTarget: &rpc.TVolumeUnlinkRequest{
			Path:      &path,
			Container: optString(container),
			Target:    optString(target),
		},
	}
	_, err := c.Call(req)
	return err
}

func (c *client) UnlinkVolumeStrict(path string, container string, target string, strict bool) error {
	req := &rpc.TContainerRequest{
		UnlinkVolume: &rpc.TVolumeUnlinkRequest{
			Path:      &path,
			Container: optString(container),
			Target:    optString(target),
			Strict:    &strict,
		},
	}
	_, err := c.Call(req)
	return err
}

// LayerAPI
func (c *client) ImportLayer(layer string, tarball string, merge bool) error {
	return c.ImportLayer4(layer, tarball, merge, "", "")
}

func (c *client) ImportLayer4(layer string, tarball string, merge bool,
	place string, privateValue string) error {
	req := &rpc.TContainerRequest{
		ImportLayer: &rpc.TLayerImportRequest{
			Layer:        &layer,
			Tarball:      &tarball,
			Merge:        &merge,
			PrivateValue: &privateValue,
		},
	}

	if place != "" {
		req.ImportLayer.Place = &place
	}

	_, err := c.Call(req)
	return err
}

func (c *client) ExportLayer(volume string, tarball string) error {
	req := &rpc.TContainerRequest{
		ExportLayer: &rpc.TLayerExportRequest{
			Volume:  &volume,
			Tarball: &tarball,
		},
	}
	_, err := c.Call(req)
	return err
}

func (c *client) RemoveLayer(layer string) error {
	return c.RemoveLayer2(layer, "")
}

func (c *client) RemoveLayer2(layer string, place string) error {
	req := &rpc.TContainerRequest{
		RemoveLayer: &rpc.TLayerRemoveRequest{
			Layer: &layer,
		},
	}

	if place != "" {
		req.RemoveLayer.Place = &place
	}

	_, err := c.Call(req)
	return err
}

func (c *client) ListLayers() ([]string, error) {
	req := &rpc.TContainerRequest{
		ListLayers: &rpc.TLayerListRequest{},
	}

	resp, err := c.Call(req)
	if err != nil {
		return nil, err
	}

	return resp.GetLayers().GetLayer(), nil
}

func (c *client) ListLayers2(place string, mask string) (ret []TLayerDescription, err error) {
	req := &rpc.TContainerRequest{
		ListLayers: &rpc.TLayerListRequest{},
	}

	if place != "" {
		req.ListLayers.Place = &place
	}

	if mask != "" {
		req.ListLayers.Mask = &mask
	}

	resp, err := c.Call(req)
	if err != nil {
		return nil, err
	}

	for _, layer := range resp.GetLayers().GetLayers() {
		var desc TLayerDescription

		desc.Name = layer.GetName()
		desc.OwnerUser = layer.GetOwnerUser()
		desc.OwnerGroup = layer.GetOwnerGroup()
		desc.LastUsage = layer.GetLastUsage()
		desc.PrivateValue = layer.GetPrivateValue()

		ret = append(ret, desc)
	}

	return ret, nil
}

func (c *client) GetLayerPrivate(layer string, place string) (string, error) {
	req := &rpc.TContainerRequest{
		Getlayerprivate: &rpc.TLayerGetPrivateRequest{
			Layer: &layer,
		},
	}

	if place != "" {
		req.Getlayerprivate.Place = &place
	}

	resp, err := c.Call(req)
	if err != nil {
		return "", err
	}

	return resp.GetLayerPrivate().GetPrivateValue(), nil
}

func (c *client) SetLayerPrivate(layer string, place string,
	privateValue string) error {
	req := &rpc.TContainerRequest{
		Setlayerprivate: &rpc.TLayerSetPrivateRequest{
			Layer:        &layer,
			PrivateValue: &privateValue,
		},
	}

	if place != "" {
		req.Setlayerprivate.Place = &place
	}

	_, err := c.Call(req)
	return err
}

func (c *client) ListStorage(place string, mask string) (ret []TStorageDescription, err error) {
	req := &rpc.TContainerRequest{
		ListStorage: &rpc.TStorageListRequest{},
	}

	if place != "" {
		req.ListStorage.Place = &place
	}

	if mask != "" {
		req.ListStorage.Mask = &mask
	}

	resp, err := c.Call(req)
	if err != nil {
		return nil, err
	}

	for _, storage := range resp.GetStorageList().GetStorages() {
		var desc TStorageDescription

		desc.Name = storage.GetName()
		desc.OwnerUser = storage.GetOwnerUser()
		desc.OwnerGroup = storage.GetOwnerGroup()
		desc.LastUsage = storage.GetLastUsage()
		desc.PrivateValue = storage.GetPrivateValue()

		ret = append(ret, desc)
	}

	return ret, nil
}

func (c *client) RemoveStorage(name string, place string) error {
	req := &rpc.TContainerRequest{
		RemoveStorage: &rpc.TStorageRemoveRequest{
			Name: &name,
		},
	}

	if place != "" {
		req.RemoveStorage.Place = &place
	}

	_, err := c.Call(req)
	return err
}

func (c *client) ConvertPath(path string, src string, dest string) (string, error) {
	req := &rpc.TContainerRequest{
		ConvertPath: &rpc.TConvertPathRequest{
			Path:        &path,
			Source:      &src,
			Destination: &dest,
		},
	}

	resp, err := c.Call(req)
	if err != nil {
		return "", err
	}

	return resp.GetConvertPath().GetPath(), nil
}

func (c *client) AttachProcess(name string, pid uint32, comm string) error {
	req := &rpc.TContainerRequest{
		AttachProcess: &rpc.TAttachProcessRequest{
			Name: &name,
			Pid:  &pid,
			Comm: &comm,
		},
	}

	_, err := c.Call(req)
	return err
}

func (c *client) DockerImageStatus(name, place string) (*rpc.TDockerImage, error) {
	req := &rpc.TContainerRequest{
		DockerImageStatus: &rpc.TDockerImageStatusRequest{
			Name: &name,
		},
	}

	if place != "" {
		req.DockerImageStatus.Place = &place
	}

	rsp, err := c.Call(req)
	if err != nil {
		return nil, err
	}

	return rsp.GetDockerImageStatus().GetImage(), nil
}

func (c *client) ListDockerImages(place, mask string) ([]*rpc.TDockerImage, error) {
	req := &rpc.TContainerRequest{
		ListDockerImages: &rpc.TDockerImageListRequest{},
	}

	if place != "" {
		req.ListDockerImages.Place = &place
	}
	if mask != "" {
		req.ListDockerImages.Mask = &mask
	}

	rsp, err := c.Call(req)
	if err != nil {
		return nil, err
	}

	return rsp.GetListDockerImages().GetImages(), nil
}

func (c *client) PullDockerImage(image DockerImage, creds DockerRegistryCredentials) (*rpc.TDockerImage, error) {
	req := &rpc.TContainerRequest{
		PullDockerImage: &rpc.TDockerImagePullRequest{
			Name: &image.Name,
		},
	}

	if image.Place != "" {
		req.PullDockerImage.Place = &image.Place
	}
	if creds.AuthToken != "" {
		req.PullDockerImage.AuthToken = &creds.AuthToken
	}
	if creds.AuthPath != "" {
		req.PullDockerImage.AuthPath = &creds.AuthPath
	}
	if creds.AuthService != "" {
		req.PullDockerImage.AuthService = &creds.AuthService
	}
	if image.Platform != "" {
		req.PullDockerImage.Platform = &image.Platform
	}
	rsp, err := c.Call(req)
	if err != nil {
		return nil, err
	}

	return rsp.GetPullDockerImage().GetImage(), nil
}

func (c *client) RemoveDockerImage(name, place string) error {
	req := &rpc.TContainerRequest{
		RemoveDockerImage: &rpc.TDockerImageRemoveRequest{
			Name: &name,
		},
	}

	if place != "" {
		req.RemoveDockerImage.Place = &place
	}

	_, err := c.Call(req)
	return err
}
